/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/wal.c
 *	Write-ahead log with a group-commit writer process.  See wal.h
 *	and the design notes (stage 1).
 *
 *	The writer owns the file; committers send records and park on an
 *	ack.  A batch is drained from the mailbox -- the first record
 *	blocks, then more are gathered with a TIMED receive until the
 *	window closes or the batch cap is hit -- and committed with one
 *	pwrite + one fdatasync, both issued via libxtc async file I/O
 *	(xtc_aio): native io_uring completion where the writer's loop
 *	supports it, a blocking-pool offload elsewhere.  Either way the
 *	writer parks while the loop keeps running, and on io_uring the
 *	write and the fsync ride the same ring with no pool thread.
 *
 *	Dogfood note: this is implemented on raw xtc_proc send/recv, not
 *	xtc_svr, because group commit is the gen_server:reply/2 pattern
 *	(stash the requester, reply later, reply to many) and xtc_svr has
 *	no deferred reply -- its xtc_svr_call_t is stack-scoped to one
 *	handle_call.
 */

#include "wal.h"

#include "xtc_aio.h"
#include "xtc_sim.h"      /* XTC_SIM_BUGGIFY -- DST pessimal batching (no-op in prod) */
#include "xtc_dst_inject.h" /* DST bug-injection harness (no-op in prod) */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>      /* snprintf, rename */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Wire kinds for a message to the writer. */
#define WAL_KIND_COMMIT  'C'
#define WAL_KIND_STOP    'S'

#define WAL_REC_HDR      12u     /* on-disk per-record header: u64 lsn + u32 len */
#define WAL_REC_CRC      8u      /* on-disk per-record trailer: u64 checksum */

/*
 * Per-record on-disk layout is now [u64 lsn][u32 len][body:len][u64 crc].
 * The trailer is a 64-bit FNV-1a hash over the header bytes (lsn+len)
 * and the body.  It makes every record self-checking: recovery can
 * tell a COMPLETE record from a torn one instead of decoding garbage.
 *
 * Torn-tail detection (Increment 1, the root fix): a crash mid-append
 * leaves a partial trailing record whose 4-byte length may read as a
 * plausible-but-wrong value < WAL_MAX_REC.  Without a checksum the scan
 * trusted that length and handed the garbage body to the decoder, whose
 * length/id fields then drove unbounded allocations (multi-GB balloon /
 * OOM on a genuinely torn STEAL base).  With
 * the trailer, wal_scan / wal_scan_tail recompute the checksum and
 * treat a MISMATCH exactly as end-of-log: the scan stops at the torn
 * tail and no torn record ever reaches xl_parse_*.  Both xstore_recover
 * and xstore_recover_inplace run through this verified scan, so neither
 * can decode a torn record.  This is the standard ARIES/Stasis
 * technique (a log record is self-checking).
 *
 * Group commit: the checksum is over each LOGICAL record, not the
 * fsync'd batch -- batch_add stamps a trailer per record before they
 * share one pwrite+fdatasync, so a batch that is torn mid-write fails
 * the checksum of exactly the first incomplete record and the scan
 * stops there, keeping every complete record ahead of it.
 *
 * This is an example engine with no external on-disk-format compat
 * requirement, so the format bump needs no version negotiation.
 */

/* 64-bit FNV-1a over the record header (lsn+len) and body.  Not
 * cryptographic -- it only needs to catch a torn/partial tail record,
 * where the trailing 8 bytes are either missing (short read) or do not
 * match a body the writer never finished.  One pass, no table, no
 * dependency. */
static uint64_t
wal_rec_crc(uint64_t lsn, uint32_t len, const void *body)
{
	const uint8_t *bp = body;
	uint64_t h = 1469598103934665603ull;    /* FNV offset basis */
	uint8_t hdr[WAL_REC_HDR];
	uint32_t i;

	memcpy(hdr, &lsn, 8);
	memcpy(hdr + 8, &len, 4);
	for (i = 0; i < WAL_REC_HDR; i++)
		h = (h ^ hdr[i]) * 1099511628211ull;   /* FNV prime */
	for (i = 0; i < len; i++)
		h = (h ^ bp[i]) * 1099511628211ull;
	return h;
}

/*
 * Upper bound on a single on-disk record body.  A legitimate record is
 * assembled in the 64 KiB batch buffer (bcap), so anything far above
 * that is a torn or corrupt record whose length field is garbage.
 * Recovery must NOT realloc(garbage_len) -- a torn STEAL base can carry
 * a partially written tail record whose 4-byte length reads as up to
 * ~4 GiB, and blindly allocating it OOMs the process (found by the
 * STEAL crash-recovery probe).  A record whose length exceeds this
 * bound (or the bytes remaining in the file) is treated as a torn
 * tail: the scan stops there, exactly as it does for a short read. */
#define WAL_MAX_REC      (16u * 1024u * 1024u)

struct wal_msg {                 /* committer -> writer */
	uint8_t   kind;
	xtc_pid_t reply_to;
	uint64_t  txn_id;
	uint32_t  len;
	uint8_t   data[];            /* len bytes */
};

struct wal_ack {                 /* writer -> committer */
	uint64_t txn_id;
	uint64_t lsn;
};

struct wal_pending {             /* one committer awaiting the current batch */
	xtc_pid_t reply_to;
	uint64_t  txn_id;
	uint64_t  lsn;
};

struct wal {
	int        fd;
	off_t      off;              /* current append offset */
	int64_t    window_ns;
	uint32_t   max_batch;
	xtc_pid_t  writer_pid;
	_Atomic int writer_alive;    /* writer proc live; cleared on its exit */

	/* Batch state -- touched only by the writer process. */
	uint8_t           *bbuf;
	size_t             bcap;
	size_t             blen;
	struct wal_pending *pend;
	uint32_t           pcount;
	uint64_t           next_lsn;
	_Atomic uint64_t   durable_lsn;

	pthread_mutex_t    sync_mu;   /* serializes wal_commit_sync appends */

	/* Stats. */
	uint64_t s_commits;
	uint64_t s_batches;
	uint64_t s_bytes;
	uint64_t s_maxbatch;
};

/* Scan an open log fd to its end: *off_out := offset just past the last
 * COMPLETE record (a torn tail is excluded), *maxlsn_out := highest LSN
 * seen.  Used to resume appending to an existing log. */
static void
wal_scan_tail(int fd, off_t *off_out, uint64_t *maxlsn_out)
{
	uint8_t *sb = NULL; size_t sbcap = 0; off_t o = 0; uint64_t maxlsn = 0;
	for (;;) {
		uint8_t hdr[WAL_REC_HDR];
		uint8_t trl[WAL_REC_CRC];
		uint64_t lsn, crc, want;
		uint32_t len;
		if (pread(fd, hdr, WAL_REC_HDR, o) != (ssize_t)WAL_REC_HDR)
			break;                /* EOF or torn header */
		memcpy(&lsn, hdr, 8); memcpy(&len, hdr + 8, 4);
		if (len > WAL_MAX_REC)
			break;                /* garbage length: torn tail */
		if (len > sbcap) {
			uint8_t *nb = realloc(sb, len);
			if (nb == NULL) break;
			sb = nb; sbcap = len;
		}
		if (len > 0 &&
		    pread(fd, sb, len, o + (off_t)WAL_REC_HDR) != (ssize_t)len)
			break;                /* torn body: stop at the tail */
		if (pread(fd, trl, WAL_REC_CRC,
		    o + (off_t)WAL_REC_HDR + (off_t)len) != (ssize_t)WAL_REC_CRC)
			break;                /* torn trailer: stop at the tail */
		memcpy(&crc, trl, WAL_REC_CRC);
		want = wal_rec_crc(lsn, len, sb);
		if (crc != want)
			break;                /* checksum mismatch: torn tail */
		if (lsn > maxlsn) maxlsn = lsn;
		o += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
	}
	free(sb);
	*off_out = o;
	*maxlsn_out = maxlsn;
}

int
wal_open(const wal_opts_t *opts, wal_t **out)
{
	wal_t *w;

	if (opts == NULL || opts->path == NULL || out == NULL)
		return XTC_E_INVAL;
	w = calloc(1, sizeof *w);
	if (w == NULL)
		return XTC_E_NOMEM;
	w->fd = open(opts->path,
	    O_RDWR | O_CREAT | (opts->append ? 0 : O_TRUNC), 0600);
	if (w->fd < 0) {
		free(w);
		return XTC_E_INTERNAL;   /* no XTC_E_IO in the core enum -- see gaps ledger */
	}
	w->off = 0;
	if (opts->append) {
		/* Continue an existing log: resume LSNs past the highest on
		 * disk, and append after the last COMPLETE record (a torn tail
		 * from a crash mid-append is dropped). */
		off_t o = 0; uint64_t maxlsn = 0;
		wal_scan_tail(w->fd, &o, &maxlsn);
		w->next_lsn = maxlsn;
		w->off = o;
		atomic_store_explicit(&w->durable_lsn, w->next_lsn, memory_order_relaxed);
		{
			int tr = ftruncate(w->fd, o);  /* drop torn tail */
			(void)tr;                      /* non-fatal: appends overwrite it */
		}
	}
	w->window_ns = opts->window_ns > 0 ? opts->window_ns : 500000;   /* 0.5ms */
	w->max_batch = opts->max_batch > 0 ? opts->max_batch : 256;
	w->bcap = 64 * 1024;
	w->bbuf = malloc(w->bcap);
	w->pend = calloc(w->max_batch, sizeof *w->pend);
	if (w->bbuf == NULL || w->pend == NULL) {
		free(w->bbuf); free(w->pend); close(w->fd); free(w);
		return XTC_E_NOMEM;
	}
	(void)pthread_mutex_init(&w->sync_mu, NULL);
	*out = w;
	return XTC_OK;
}

void
wal_close(wal_t *w)
{
	if (w == NULL)
		return;
	(void)pthread_mutex_destroy(&w->sync_mu);
	if (w->fd >= 0)
		close(w->fd);
	free(w->bbuf);
	free(w->pend);
	free(w);
}

/* Append one record to the current batch buffer (writer proc only). */
static int
batch_add(wal_t *w, const uint8_t *data, uint32_t len,
    xtc_pid_t reply_to, uint64_t txn_id)
{
	size_t need = WAL_REC_HDR + len + WAL_REC_CRC;
	uint64_t lsn, crc;

	if (w->blen + need > w->bcap) {
		size_t ncap = w->bcap * 2;
		uint8_t *nb;
		while (ncap < w->blen + need)
			ncap *= 2;
		nb = realloc(w->bbuf, ncap);
		if (nb == NULL)
			return XTC_E_NOMEM;
		w->bbuf = nb;
		w->bcap = ncap;
	}
	lsn = ++w->next_lsn;
	memcpy(w->bbuf + w->blen, &lsn, 8);
	memcpy(w->bbuf + w->blen + 8, &len, 4);
	memcpy(w->bbuf + w->blen + WAL_REC_HDR, data, len);
	crc = wal_rec_crc(lsn, len, data);
	memcpy(w->bbuf + w->blen + WAL_REC_HDR + len, &crc, WAL_REC_CRC);
	w->blen += need;
	w->pend[w->pcount].reply_to = reply_to;
	w->pend[w->pcount].txn_id = txn_id;
	w->pend[w->pcount].lsn = lsn;
	w->pcount++;
	w->s_bytes += len;
	return XTC_OK;
}

/* Durably write the current batch, then ack every committer in it. */
static void
batch_flush(wal_t *w)
{
	struct wal_ack ack;
	int rc;
	uint32_t i;

	if (w->pcount == 0)
		return;

	/*
	 * Durably write the batch via libxtc async file I/O: native
	 * io_uring completion when the writer runs on a capable loop, the
	 * blocking pool otherwise.  The writer parks while the loop keeps
	 * serving other work -- and on io_uring the batch write and its
	 * fdatasync ride the same ring with no pool thread involved.
	 */
	rc = xtc_aio_pwrite(w->fd, w->bbuf, (uint32_t)w->blen, (int64_t)w->off);
#if XTC_DST_BUG(XTC_DST_BUG_NODURABLE)
	(void)rc;   /* planted bug: skip the fdatasync but still ack below ->
	             * an acked commit is not durable across a crash */
#else
	rc = (rc == (int)w->blen) ? xtc_aio_fdatasync(w->fd) : -1;
#endif
	(void)rc;                            /* a real engine would surface I/O errors */

	w->off += (off_t)w->blen;
	atomic_store_explicit(&w->durable_lsn, w->pend[w->pcount - 1].lsn, memory_order_relaxed);
	w->s_batches++;
	w->s_commits += w->pcount;
	if (w->pcount > w->s_maxbatch)
		w->s_maxbatch = w->pcount;

	for (i = 0; i < w->pcount; i++) {
		ack.txn_id = w->pend[i].txn_id;
		ack.lsn = w->pend[i].lsn;
		(void)xtc_send(w->pend[i].reply_to, &ack, sizeof ack);
	}
	w->blen = 0;
	w->pcount = 0;
}

static void
wal_writer_proc(void *arg)
{
	wal_t *w = arg;

	for (;;) {
		void *m = NULL;
		size_t n = 0;
		struct wal_msg *msg;
		int stop = 0;

		/* Block for the first record of a batch. */
		if (xtc_recv(&m, &n, -1) != XTC_OK || m == NULL)
			continue;
		msg = m;
		if (msg->kind == WAL_KIND_STOP) {
			free(m);
			break;
		}
		(void)batch_add(w, msg->data, msg->len, msg->reply_to, msg->txn_id);
		free(m);

		/*
		 * Pipelined group commit: drain everything already queued
		 * (non-blocking) and flush it at once -- no fixed gather window.
		 * batch_flush parks the writer on the fsync, so commits that
		 * arrive while that fsync is in flight queue in the mailbox and
		 * form the next batch.  The batch size therefore self-tunes to
		 * the fsync latency x arrival rate: a lone committer pays just
		 * one fsync (no added window latency -- which on fast storage
		 * would otherwise make group commit lose to a direct synchronous
		 * commit), while under load many commits coalesce per fsync.
		 */
		while (w->pcount < w->max_batch) {
			/* Buggify: flush a TINY batch instead of coalescing all
			 * queued commits.  Group commit makes no maximal-batching
			 * promise -- each committer is still durably fsync'd -- so
			 * stopping the drain early is legal, and it stresses
			 * crash-recovery with more, smaller batches (more fsync /
			 * torn-tail boundaries).  Per-call BUGGIFY coin (fixed per
			 * site per run, replays); a no-op in production. */
			if (XTC_SIM_BUGGIFY("wal.flush.tiny_batch") &&
			    xtc_sim_buggify_fault(300))
				break;
			m = NULL; n = 0;
			if (xtc_recv(&m, &n, 0) != XTC_OK || m == NULL)
				break;               /* nothing more queued right now */
			msg = m;
			if (msg->kind == WAL_KIND_STOP) {
				stop = 1;
				free(m);
				break;
			}
			(void)batch_add(w, msg->data, msg->len, msg->reply_to,
			    msg->txn_id);
			free(m);
		}

		batch_flush(w);
		if (stop)
			break;
	}
	atomic_store_explicit(&w->writer_alive, 0, memory_order_release);
}

int
wal_writer_spawn(wal_t *w, xtc_loop_t *loop, xtc_pid_t *pid)
{
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t p;
	int rc;

	if (w == NULL || loop == NULL)
		return XTC_E_INVAL;
	o.name = "wal";
	atomic_store_explicit(&w->writer_alive, 1, memory_order_release);
	rc = xtc_proc_spawn(loop, wal_writer_proc, w, &o, &p);
	if (rc != XTC_OK) {
		atomic_store_explicit(&w->writer_alive, 0, memory_order_release);
		return rc;
	}
	w->writer_pid = p;
	if (pid != NULL)
		*pid = p;
	return XTC_OK;
}

xtc_pid_t
wal_writer_pid(const wal_t *w)
{
	return w->writer_pid;
}

int
wal_commit(wal_t *w, const void *record, uint32_t len, uint64_t *lsn)
{
	struct wal_msg *msg;
	struct wal_ack *ack;
	void *am = NULL;
	size_t an = 0;
	size_t msz;
	uint64_t txn_id;
	int rc;

	if (w == NULL || (record == NULL && len != 0))
		return XTC_E_INVAL;

	msz = sizeof *msg + len;
	msg = malloc(msz);
	if (msg == NULL)
		return XTC_E_NOMEM;
	msg->kind = WAL_KIND_COMMIT;
	msg->reply_to = xtc_self();
	/* A per-committer correlation id; the writer echoes it back. */
	txn_id = ((uint64_t)(uintptr_t)msg);
	msg->txn_id = txn_id;
	msg->len = len;
	if (len)
		memcpy(msg->data, record, len);

	rc = xtc_send(w->writer_pid, msg, msz);
	free(msg);
	if (rc != XTC_OK)
		return rc;

	/* Park on the ack.  In this stage the committer receives only
	 * acks, so a plain recv suffices; a committer that also receives
	 * other traffic would use xtc_recv_correlate on txn_id. */
	rc = xtc_recv(&am, &an, -1);
	if (rc != XTC_OK)
		return rc;
	if (am == NULL || an < sizeof *ack) {
		free(am);
		return XTC_E_INTERNAL;
	}
	ack = am;
	if (lsn != NULL)
		*lsn = ack->lsn;
	free(am);
	return XTC_OK;
}

int
wal_commit_sync(wal_t *w, const void *record, uint32_t len, uint64_t *lsn)
{
	uint8_t hdr[WAL_REC_HDR];
	uint8_t trl[WAL_REC_CRC];
	uint64_t my_lsn, crc;
	size_t done;
	int rc = XTC_OK;

	if (w == NULL || (record == NULL && len != 0))
		return XTC_E_INVAL;

	(void)pthread_mutex_lock(&w->sync_mu);
	my_lsn = ++w->next_lsn;
	memcpy(hdr, &my_lsn, 8);
	memcpy(hdr + 8, &len, 4);
	/* header + payload at the current append offset */
	if (pwrite(w->fd, hdr, WAL_REC_HDR, w->off) != (ssize_t)WAL_REC_HDR) {
		rc = XTC_E_INTERNAL; goto out;
	}
	for (done = 0; done < len; ) {
		ssize_t n = pwrite(w->fd, (const uint8_t *)record + done, len - done,
		    w->off + (off_t)WAL_REC_HDR + (off_t)done);
		if (n < 0) { if (errno == EINTR) continue; rc = XTC_E_INTERNAL; goto out; }
		done += (size_t)n;
	}
	/* self-checking trailer so recovery can drop a torn tail */
	crc = wal_rec_crc(my_lsn, len, record);
	memcpy(trl, &crc, WAL_REC_CRC);
	if (pwrite(w->fd, trl, WAL_REC_CRC,
	    w->off + (off_t)WAL_REC_HDR + (off_t)len) != (ssize_t)WAL_REC_CRC) {
		rc = XTC_E_INTERNAL; goto out;
	}
	if (fdatasync(w->fd) != 0) { rc = XTC_E_INTERNAL; goto out; }
	w->off += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
	atomic_store_explicit(&w->durable_lsn, my_lsn, memory_order_relaxed);
	w->s_commits++;
	w->s_batches++;
	w->s_bytes += len;
	if (lsn != NULL) *lsn = my_lsn;
out:
	(void)pthread_mutex_unlock(&w->sync_mu);
	return rc;
}

/* Rebind an open log handle to `path` after it was replaced on disk
 * (the compaction rename): reopen the fd and resume appending past the
 * last complete record.  Caller must hold no concurrent committers. */
static int
wal_rebind(wal_t *w, const char *path)
{
	off_t o = 0; uint64_t maxlsn = 0;
	int fd = open(path, O_RDWR, 0600);
	if (fd < 0)
		return XTC_E_INTERNAL;
	wal_scan_tail(fd, &o, &maxlsn);
	if (w->fd >= 0) close(w->fd);
	w->fd = fd;
	w->off = o;
	w->next_lsn = maxlsn;
	atomic_store_explicit(&w->durable_lsn, maxlsn, memory_order_relaxed);
	return XTC_OK;
}

/* Emit context for wal_checkpoint: appends framed records to the temp
 * compaction file, assigning fresh sequential LSNs. */
struct wal_cmp_ctx { int fd; off_t off; uint64_t lsn; int err; };
static void
wal_cmp_emit(void *vctx, const void *payload, uint32_t len)
{
	struct wal_cmp_ctx *c = vctx;
	uint8_t hdr[WAL_REC_HDR];
	uint8_t trl[WAL_REC_CRC];
	uint64_t lsn = ++c->lsn;
	uint64_t crc;
	if (c->err)
		return;
	memcpy(hdr, &lsn, 8);
	memcpy(hdr + 8, &len, 4);
	crc = wal_rec_crc(lsn, len, payload);
	memcpy(trl, &crc, WAL_REC_CRC);
	if (pwrite(c->fd, hdr, WAL_REC_HDR, c->off) != (ssize_t)WAL_REC_HDR ||
	    (len && pwrite(c->fd, payload, len, c->off + WAL_REC_HDR) != (ssize_t)len) ||
	    pwrite(c->fd, trl, WAL_REC_CRC,
	    c->off + WAL_REC_HDR + len) != (ssize_t)WAL_REC_CRC) {
		c->err = 1;
		return;
	}
	c->off += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
}

int
wal_checkpoint(wal_t *w, const char *path,
    void (*dump)(wal_emit_fn emit, void *emit_ctx, void *user), void *user)
{
	char tmp[1100];
	struct wal_cmp_ctx c;
	int fd, dfd;

	if (w == NULL || path == NULL)
		return XTC_E_INVAL;
	if ((int)strlen(path) + 9 >= (int)sizeof tmp)
		return XTC_E_INVAL;
	snprintf(tmp, sizeof tmp, "%s.compact", path);
	fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return XTC_E_INTERNAL;
	c.fd = fd; c.off = 0; c.lsn = 0; c.err = 0;

	/* The dump emits every record of the compacted log, starting with
	 * its own checkpoint record -- this layer does not interpret the
	 * record bytes. */
	if (dump != NULL)
		dump(wal_cmp_emit, &c, user);

	if (c.err || fsync(fd) != 0) {
		(void)close(fd);
		(void)unlink(tmp);
		return XTC_E_INTERNAL;
	}
	(void)close(fd);
	if (rename(tmp, path) != 0) {     /* atomic replace of the live log */
		(void)unlink(tmp);
		return XTC_E_INTERNAL;
	}
	/* Make the rename durable so a crash cannot resurrect the old log. */
	{
		char dir[1100];
		char *slash;
		snprintf(dir, sizeof dir, "%s", path);
		slash = strrchr(dir, '/');
		if (slash != NULL) *slash = '\0'; else { dir[0] = '.'; dir[1] = '\0'; }
		dfd = open(dir, O_RDONLY);
		if (dfd >= 0) { (void)fsync(dfd); (void)close(dfd); }
	}
	return wal_rebind(w, path);
}

int
wal_scan(const char *path, wal_replay_cb cb, void *user)
{
	int fd;
	off_t off = 0;
	uint8_t *buf = NULL;
	size_t bufcap = 0;
	int rc = XTC_OK;

	if (path == NULL || cb == NULL)
		return XTC_E_INVAL;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return XTC_OK;                /* no log yet: nothing to replay */
	for (;;) {
		uint8_t hdr[WAL_REC_HDR];
		uint8_t trl[WAL_REC_CRC];
		uint64_t lsn, crc, want;
		uint32_t len;
		ssize_t n = pread(fd, hdr, WAL_REC_HDR, off);
		if (n != (ssize_t)WAL_REC_HDR)
			break;                   /* EOF or torn header: stop at the tail */
		memcpy(&lsn, hdr, 8);
		memcpy(&len, hdr + 8, 4);
		if (len > WAL_MAX_REC)
			break;                /* garbage length: torn tail, stop */
		if (len > bufcap) {
			uint8_t *nb = realloc(buf, len);
			if (nb == NULL) { rc = XTC_E_NOMEM; break; }
			buf = nb; bufcap = len;
		}
		n = pread(fd, buf, len, off + (off_t)WAL_REC_HDR);
		if (n != (ssize_t)len)
			break;                   /* torn record body: stop (crash tail) */
		n = pread(fd, trl, WAL_REC_CRC,
		    off + (off_t)WAL_REC_HDR + (off_t)len);
		if (n != (ssize_t)WAL_REC_CRC)
			break;                   /* torn trailer: stop (crash tail) */
		memcpy(&crc, trl, WAL_REC_CRC);
		want = wal_rec_crc(lsn, len, buf);
		if (crc != want)
			break;                   /* checksum mismatch: torn tail, stop */
		if (cb(lsn, buf, len, user) != 0)
			break;
		off += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
	}
	free(buf);
	close(fd);
	return rc;
}

int
wal_truncate(wal_t *w)
{
	if (w == NULL)
		return XTC_E_INVAL;
	(void)pthread_mutex_lock(&w->sync_mu);
	if (ftruncate(w->fd, 0) != 0) {
		(void)pthread_mutex_unlock(&w->sync_mu);
		return XTC_E_INTERNAL;
	}
	w->off = 0;            /* next append at offset 0; next_lsn keeps rising */
	(void)pthread_mutex_unlock(&w->sync_mu);
	return XTC_OK;
}

uint64_t
wal_durable_lsn(const wal_t *w)
{
	if (w == NULL)
		return 0;
	return atomic_load_explicit(&w->durable_lsn, memory_order_relaxed);
}

int
wal_fd(const wal_t *w)
{
	return (w == NULL) ? -1 : w->fd;
}

int
wal_flush_through(wal_t *w, uint64_t lsn)
{
	if (w == NULL)
		return XTC_E_INVAL;
	/* The change at `lsn` was logged-and-acked before the page that
	 * carries it was dirtied, so the log is already durable this far in
	 * the current commit-before-apply protocol; report whether it is.
	 * (When uncommitted pages may be stolen to disk, this is where the
	 * log would be forced.) */
	if (atomic_load_explicit(&w->durable_lsn, memory_order_relaxed) >= lsn)
		return XTC_OK;
	return XTC_E_AGAIN;
}

int
wal_writer_stop(wal_t *w)
{
	uint8_t kind = WAL_KIND_STOP;
	int rc, i;

	if (w == NULL)
		return XTC_E_INVAL;
	rc = xtc_send(w->writer_pid, &kind, sizeof kind);
	/* Wait for the writer to drain its batch and exit before a caller
	 * (wal_close) frees the log out from under it. */
	for (i = 0; i < 100000 &&
	    atomic_load_explicit(&w->writer_alive, memory_order_acquire); i++)
		if (xtc_proc_sleep(200LL * 1000) != XTC_OK) break;
	return rc;
}

void
wal_get_stats(wal_t *w, wal_stats_t *out)
{
	if (w == NULL || out == NULL)
		return;
	out->commits = w->s_commits;
	out->batches = w->s_batches;
	out->bytes = w->s_bytes;
	out->max_batch_seen = w->s_maxbatch;
	out->durable_lsn = atomic_load_explicit(&w->durable_lsn, memory_order_relaxed);
}
