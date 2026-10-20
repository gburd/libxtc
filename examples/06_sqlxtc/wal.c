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
	int32_t  status;             /* XTC_OK, or the error that failed the batch */
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
	/* Byte offset of the durable end of the file (always a frame
	 * boundary), published with durable_lsn so an online checkpoint can
	 * copy the durable tail from another thread; and a generation bumped
	 * whenever the handle is rebound to a new file or truncated, so one
	 * that raced a quiesced rewrite aborts instead of renaming over it. */
	_Atomic uint64_t   durable_off;
	_Atomic uint64_t   gen;
	/* Fail-stop latch.  Set on the first failed or short log write or
	 * fdatasync; once set, nothing more is written and every commit
	 * fails with XTC_E_IO until the log is reopened (process restart).
	 * After a failed fsync the kernel may have dropped the dirty pages
	 * it could not write, so retrying cannot make the earlier records
	 * durable (PostgreSQL's fsync lesson): stop instead. */
	_Atomic int        failed;

	pthread_mutex_t    sync_mu;   /* guards the group-commit state below */
	/* Off-loop group commit (wal_commit_sync).  A committer appends its
	 * frame to gbuf and takes the next LSN under sync_mu, then waits
	 * until durable_lsn covers it.  The first one to find no flush in
	 * progress becomes the leader: it swaps gbuf out, drops the mutex,
	 * writes and fdatasyncs the whole batch, publishes durable_lsn and
	 * wakes everyone.  Frames that arrive during a flush form the next
	 * batch, so N concurrent committers share ~1 fsync instead of paying
	 * N in series (the deferred-durable commit of the PostgreSQL async
	 * WAL series, with the leader doing the flush instead of a
	 * notifier).  A lone committer still pays exactly one fsync. */
	pthread_cond_t     sync_cv;
	int                g_flushing;
	uint8_t           *gbuf, *gspare;
	size_t             glen, gcap, gspare_cap;
	uint32_t           gcount;     /* frames in gbuf */

	/* Stats. */
	uint64_t s_commits;
	uint64_t s_batches;
	uint64_t s_bytes;
	uint64_t s_maxbatch;
	uint64_t s_failed;            /* commits refused by a failed log write */
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
		atomic_store_explicit(&w->durable_off, (uint64_t)o, memory_order_relaxed);
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
	(void)pthread_cond_init(&w->sync_cv, NULL);
	*out = w;
	return XTC_OK;
}

void
wal_close(wal_t *w)
{
	if (w == NULL)
		return;
	(void)pthread_cond_destroy(&w->sync_cv);
	(void)pthread_mutex_destroy(&w->sync_mu);
	free(w->gbuf);
	free(w->gspare);
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

/* Durably write the current batch, then ack every committer in it.
 * On a failed or short write, or a failed fdatasync, neither the append
 * offset nor durable_lsn moves, every committer in the batch gets an
 * XTC_E_IO ack, and the log latches failed (see struct wal). */
static void
batch_flush(wal_t *w)
{
	struct wal_ack ack;
	int rc, status = XTC_OK;
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
	if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
		status = XTC_E_IO;
	} else {
		rc = xtc_aio_pwrite(w->fd, w->bbuf, (uint32_t)w->blen,
		    (int64_t)w->off);
		if (rc != (int)w->blen)
			status = XTC_E_IO;
#if XTC_DST_BUG(XTC_DST_BUG_NODURABLE)
		/* planted bug: skip the fdatasync but still ack below -> an
		 * acked commit is not durable across a crash */
#else
		else if (xtc_aio_fdatasync(w->fd) != 0)
			status = XTC_E_IO;
#endif
	}

	if (status == XTC_OK) {
		w->off += (off_t)w->blen;
		atomic_store_explicit(&w->durable_off, (uint64_t)w->off,
		    memory_order_release);
		atomic_store_explicit(&w->durable_lsn,
		    w->pend[w->pcount - 1].lsn, memory_order_release);
		w->s_batches++;
		w->s_commits += w->pcount;
		if (w->pcount > w->s_maxbatch)
			w->s_maxbatch = w->pcount;
	} else {
		atomic_store_explicit(&w->failed, 1, memory_order_release);
		w->s_failed += w->pcount;
	}

	for (i = 0; i < w->pcount; i++) {
		ack.txn_id = w->pend[i].txn_id;
		ack.lsn = status == XTC_OK ? w->pend[i].lsn : 0;
		ack.status = status;
		/* A full mailbox (XTC_E_AGAIN, soft backpressure) must not drop
		 * the ack: the committer is parked on it forever otherwise.
		 * Any other error means the committer is gone. */
		while (xtc_send(w->pend[i].reply_to, &ack, sizeof ack) ==
		    XTC_E_AGAIN)
			(void)xtc_proc_sleep(10LL * 1000);
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
	if (atomic_load_explicit(&w->failed, memory_order_acquire))
		return XTC_E_IO;          /* fail-stop: the log is poisoned */

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

	/* The writer's mailbox is a soft cap: retry on XTC_E_AGAIN rather
	 * than fail the commit for backpressure. */
	while ((rc = xtc_send(w->writer_pid, msg, msz)) == XTC_E_AGAIN)
		(void)xtc_proc_sleep(10LL * 1000);
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
	rc = ack->status;
	if (rc == XTC_OK && lsn != NULL)
		*lsn = ack->lsn;
	free(am);
	return rc;
}

/* Caller holds sync_mu: append one framed record to gbuf, taking the
 * next LSN.  Returns the LSN, or 0 (XTC_E_NOMEM: nothing appended). */
static uint64_t
wal_gbuf_add(wal_t *w, const void *record, uint32_t len)
{
	size_t need = WAL_REC_HDR + len + WAL_REC_CRC;
	uint64_t lsn, crc;
	if (w->glen + need > w->gcap) {
		size_t ncap = w->gcap ? w->gcap : 64 * 1024;
		uint8_t *nb;
		while (ncap < w->glen + need)
			ncap *= 2;
		if ((nb = realloc(w->gbuf, ncap)) == NULL)
			return 0;
		w->gbuf = nb;
		w->gcap = ncap;
	}
	lsn = ++w->next_lsn;
	memcpy(w->gbuf + w->glen, &lsn, 8);
	memcpy(w->gbuf + w->glen + 8, &len, 4);
	if (len)
		memcpy(w->gbuf + w->glen + WAL_REC_HDR, record, len);
	crc = wal_rec_crc(lsn, len, record);
	memcpy(w->gbuf + w->glen + WAL_REC_HDR + len, &crc, WAL_REC_CRC);
	w->glen += need;
	w->gcount++;
	w->s_bytes += len;
	return lsn;
}

/* Leader, entered and left holding sync_mu with g_flushing set by the
 * caller: write + fdatasync the current batch with the mutex dropped. */
static void
wal_gbuf_flush(wal_t *w)
{
	uint8_t *buf = w->gbuf;
	size_t blen = w->glen, done = 0, cap = w->gcap;
	uint32_t count = w->gcount;
	uint64_t last = w->next_lsn;
	off_t off = w->off;
	int ok = 1;

	/* Hand the followers an empty buffer for the next batch. */
	w->gbuf = w->gspare; w->gcap = w->gspare_cap;
	w->gspare = NULL; w->gspare_cap = 0;
	w->glen = 0; w->gcount = 0;
	(void)pthread_mutex_unlock(&w->sync_mu);
	while (ok && done < blen) {
		ssize_t n = pwrite(w->fd, buf + done, blen - done, off + (off_t)done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			ok = 0;
		else
			done += (size_t)n;
	}
#if XTC_DST_BUG(XTC_DST_BUG_NODURABLE)
	/* planted bug: skip the fdatasync but still report durable */
#else
	if (ok && fdatasync(w->fd) != 0)
		ok = 0;
#endif
	(void)pthread_mutex_lock(&w->sync_mu);
	if (ok) {
		w->off = off + (off_t)blen;
		atomic_store_explicit(&w->durable_off, (uint64_t)w->off,
		    memory_order_release);
		atomic_store_explicit(&w->durable_lsn, last, memory_order_release);
		w->s_batches++;
		w->s_commits += count;
		if (count > w->s_maxbatch)
			w->s_maxbatch = count;
	} else {
		atomic_store_explicit(&w->failed, 1, memory_order_release);
		w->s_failed += count;
	}
	/* Keep the flushed buffer as the next spare (no realloc churn). */
	if (w->gspare == NULL) {
		w->gspare = buf; w->gspare_cap = cap;
	} else {
		free(buf);
	}
	w->g_flushing = 0;
	(void)pthread_cond_broadcast(&w->sync_cv);
}

int
wal_commit_sync(wal_t *w, const void *record, uint32_t len, uint64_t *lsn)
{
	uint64_t my_lsn;
	int rc = XTC_OK;

	if (w == NULL || (record == NULL && len != 0))
		return XTC_E_INVAL;

	(void)pthread_mutex_lock(&w->sync_mu);
	if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&w->sync_mu);
		return XTC_E_IO;
	}
	if ((my_lsn = wal_gbuf_add(w, record, len)) == 0) {
		(void)pthread_mutex_unlock(&w->sync_mu);
		return XTC_E_NOMEM;
	}
	for (;;) {
		if (atomic_load_explicit(&w->durable_lsn, memory_order_acquire) >= my_lsn)
			break;                   /* a leader flushed us */
		if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
			rc = XTC_E_IO;           /* our batch (or an earlier one) failed */
			break;
		}
		if (!w->g_flushing) {
			w->g_flushing = 1;       /* lead: flush everything queued */
			wal_gbuf_flush(w);
		} else {
			(void)pthread_cond_wait(&w->sync_cv, &w->sync_mu);
		}
	}
	(void)pthread_mutex_unlock(&w->sync_mu);
	if (rc == XTC_OK && lsn != NULL)
		*lsn = my_lsn;
	return rc;
}

int
wal_failed(const wal_t *w)
{
	return w != NULL && atomic_load_explicit(&w->failed, memory_order_acquire);
}

/* Rebind an open log handle to `path` after it was replaced on disk
 * (the compaction rename).  This layer just wrote the file, so its end
 * offset and highest LSN are known: no rescan (a rescan cost ~0.2 s per
 * 100 MB, all of it with commits quiesced).  Caller holds no concurrent
 * committers. */
static int
wal_rebind_at(wal_t *w, const char *path, off_t off, uint64_t lsn)
{
	int fd = open(path, O_RDWR, 0600);
	if (fd < 0)
		return XTC_E_INTERNAL;
	if (w->fd >= 0) close(w->fd);
	w->fd = fd;
	w->off = off;
	w->next_lsn = lsn;
	atomic_store_explicit(&w->durable_off, (uint64_t)off, memory_order_release);
	atomic_store_explicit(&w->durable_lsn, lsn, memory_order_release);
	atomic_fetch_add_explicit(&w->gen, 1, memory_order_acq_rel);
	return XTC_OK;
}

/* fsync the directory holding `path`, so a rename into it is durable. */
static void
wal_sync_dir(const char *path)
{
	char dir[1100];
	char *slash;
	int dfd;
	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (slash != NULL) *slash = '\0'; else { dir[0] = '.'; dir[1] = '\0'; }
	dfd = open(dir, O_RDONLY);
	if (dfd >= 0) { (void)fsync(dfd); (void)close(dfd); }
}

/* Emit context for wal_checkpoint: appends framed records to the temp
 * compaction file, assigning fresh sequential LSNs that CONTINUE the
 * log's sequence (they start just above the handle's next_lsn), so LSNs
 * never go backwards across a checkpoint -- see wal_checkpoint. */
/*
 * The records are staged in a 1 MiB buffer and written in large
 * chunks: three pwrite()s per record (header, body, trailer) made a W=1
 * TPC-C checkpoint ~2M syscalls and 1-3.5 s, all of it with commits
 * quiesced.  `off` counts bytes emitted (flushed + staged).
 */
#define WAL_CMP_BUF (1u << 20)
struct wal_cmp_ctx {
	int fd; off_t off; uint64_t lsn; int err;
	uint8_t *buf; size_t blen; off_t boff;   /* staged bytes start at boff */
};

static void
wal_cmp_flush(struct wal_cmp_ctx *c)
{
	size_t done = 0;
	while (!c->err && done < c->blen) {
		ssize_t n = pwrite(c->fd, c->buf + done, c->blen - done,
		    c->boff + (off_t)done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			c->err = 1;
		else
			done += (size_t)n;
	}
	c->boff += (off_t)c->blen;
	c->blen = 0;
}

static void
wal_cmp_put(struct wal_cmp_ctx *c, const void *p, size_t n)
{
	const uint8_t *s = p;
	while (n > 0 && !c->err) {
		size_t room = WAL_CMP_BUF - c->blen, k = n < room ? n : room;
		memcpy(c->buf + c->blen, s, k);
		c->blen += k; s += k; n -= k;
		if (c->blen == WAL_CMP_BUF)
			wal_cmp_flush(c);
	}
}

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
	wal_cmp_put(c, hdr, WAL_REC_HDR);
	if (len)
		wal_cmp_put(c, payload, len);
	wal_cmp_put(c, trl, WAL_REC_CRC);
	c->off += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
}

int
wal_checkpoint(wal_t *w, const char *path,
    void (*dump)(wal_emit_fn emit, void *emit_ctx, void *user), void *user)
{
	char tmp[1100];
	struct wal_cmp_ctx c;
	int fd;

	if (w == NULL || path == NULL || dump == NULL)
		return XTC_E_INVAL;
	if ((int)strlen(path) + 9 >= (int)sizeof tmp)
		return XTC_E_INVAL;
	snprintf(tmp, sizeof tmp, "%s.compact", path);
	fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return XTC_E_INTERNAL;
	c.fd = fd; c.off = 0; c.err = 0;
	c.blen = 0; c.boff = 0;
	c.buf = malloc(WAL_CMP_BUF);
	if (c.buf == NULL) {
		(void)close(fd);
		(void)unlink(tmp);
		return XTC_E_NOMEM;
	}
	/*
	 * Continue the LSN sequence; do not restart it.  A data page carries
	 * the LSN of the last change written to it, and recovery redoes a
	 * logged page image only if the record's LSN is NEWER than the page's
	 * (bm_apply_page_image_at).  Numbering the compacted log from 1 made
	 * every post-checkpoint record older than the pages flushed before
	 * the checkpoint, so in-place recovery refused their images and lost
	 * post-checkpoint changes on exactly those pages (test_fuzzy_checkpoint
	 * hit it once eviction order flushed tail pages differently;
	 * test_wal_lsn_monotone pins it).  wal_rebind_at resumes next_lsn from
	 * the compacted file's highest LSN, which is now above every LSN the
	 * old log handed out.
	 */
	c.lsn = w->next_lsn;

	/* The dump emits every record of the compacted log, starting with
	 * its own checkpoint record -- this layer does not interpret the
	 * record bytes. */
	dump(wal_cmp_emit, &c, user);
	wal_cmp_flush(&c);
	free(c.buf);
	/* An empty replacement cannot persist the LSN high-water mark.
	 * Reject before rename so reopen cannot restart the sequence. */
	if (!c.err && c.off == 0) {
		(void)close(fd);
		(void)unlink(tmp);
		return XTC_E_INVAL;
	}

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
	wal_sync_dir(path);
	return wal_rebind_at(w, path, c.off, c.lsn);
}

/*
 * Online checkpoint (see wal.h).  The compacted file is written exactly
 * as wal_checkpoint writes it (wal_cmp_emit), then the live log's
 * durable frames after the begin point are appended to it, re-numbered
 * so the new file's LSNs keep rising past every LSN the old file handed
 * out (each copied frame takes one new LSN, and the dump adds at least
 * one, so the final next_lsn exceeds the old one).
 */
struct wal_ock {
	wal_t   *w;
	char     path[1100], tmp[1100];
	int      src_fd;              /* the live log, read side */
	off_t    src_off;             /* next live-log byte to copy */
	uint64_t gen;                 /* w->gen at begin */
	uint64_t frames;              /* live-log frames copied */
	uint8_t *rbuf;
	size_t   rcap;
	struct wal_cmp_ctx c;
};

int
wal_ockpt_begin(wal_t *w, const char *path, wal_ock_t **out)
{
	wal_ock_t *k;
	if (w == NULL || path == NULL || out == NULL ||
	    (int)strlen(path) + 9 >= (int)sizeof k->tmp)
		return XTC_E_INVAL;
	if (atomic_load_explicit(&w->failed, memory_order_acquire))
		return XTC_E_IO;
	if ((k = calloc(1, sizeof *k)) == NULL)
		return XTC_E_NOMEM;
	k->w = w;
	k->src_fd = -1;
	k->c.fd = -1;
	snprintf(k->path, sizeof k->path, "%s", path);
	snprintf(k->tmp, sizeof k->tmp, "%s.ocompact", path);
	k->rcap = WAL_CMP_BUF;
	k->rbuf = malloc(k->rcap);
	k->c.buf = malloc(WAL_CMP_BUF);
	k->src_fd = open(path, O_RDONLY);
	k->c.fd = open(k->tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (k->rbuf == NULL || k->c.buf == NULL || k->src_fd < 0 || k->c.fd < 0) {
		wal_ockpt_abort(k);
		return XTC_E_INTERNAL;
	}
	/* Quiesced: every logged record is durable, so the durable end is
	 * the append end and the durable LSN the last one handed out. */
	k->src_off = (off_t)atomic_load_explicit(&w->durable_off,
	    memory_order_acquire);
	k->c.lsn = atomic_load_explicit(&w->durable_lsn, memory_order_acquire);
	k->gen = atomic_load_explicit(&w->gen, memory_order_acquire);
	*out = k;
	return XTC_OK;
}

void
wal_ockpt_emitter(wal_ock_t *k, wal_emit_fn *fn, void **ctx)
{
	*fn = wal_cmp_emit;
	*ctx = &k->c;
}

uint64_t
wal_ockpt_frames(const wal_ock_t *k)
{
	return k == NULL ? 0 : k->frames;
}

/* Copy the live log's frames in [src_off, end) to the compacted file,
 * re-numbered.  `end` is a durable frame boundary.  Stops after about
 * `max` bytes (0 = no limit) so a caller can check for cancellation
 * between slices. */
static int
wal_ock_copy(wal_ock_t *k, off_t end, off_t max)
{
	off_t start = k->src_off;
	while (k->src_off < end && !k->c.err &&
	    (max == 0 || k->src_off - start < max)) {
		size_t want = (size_t)(end - k->src_off), pos = 0;
		ssize_t n;
		if (want > k->rcap)
			want = k->rcap;
		n = pread(k->src_fd, k->rbuf, want, k->src_off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < (ssize_t)WAL_REC_HDR)
			return XTC_E_INTERNAL;    /* end is not a frame boundary */
		for (;;) {
			uint64_t lsn, crc;
			uint32_t len;
			size_t fl;
			if (pos + WAL_REC_HDR > (size_t)n)
				break;
			memcpy(&lsn, k->rbuf + pos, 8);
			memcpy(&len, k->rbuf + pos + 8, 4);
			if (len > WAL_MAX_REC)
				return XTC_E_INTERNAL;
			fl = WAL_REC_HDR + len + WAL_REC_CRC;
			if (pos + fl > (size_t)n) {
				if (pos == 0) {       /* one frame larger than the buffer */
					uint8_t *nb;
					if ((off_t)fl > end - k->src_off)
						return XTC_E_INTERNAL;
					if ((nb = realloc(k->rbuf, fl)) == NULL)
						return XTC_E_NOMEM;
					k->rbuf = nb;
					k->rcap = fl;
				}
				break;
			}
			memcpy(&crc, k->rbuf + pos + WAL_REC_HDR + len, WAL_REC_CRC);
			if (crc != wal_rec_crc(lsn, len, k->rbuf + pos + WAL_REC_HDR))
				return XTC_E_INTERNAL;   /* never launder a bad frame */
			wal_cmp_emit(&k->c, k->rbuf + pos + WAL_REC_HDR, len);
			k->frames++;
			pos += fl;
		}
		k->src_off += (off_t)pos;
	}
	return k->c.err ? XTC_E_INTERNAL : XTC_OK;
}

int
wal_ockpt_catchup(wal_ock_t *k, uint64_t *left)
{
	off_t end;
	int rc;
	if (atomic_load_explicit(&k->w->gen, memory_order_acquire) != k->gen)
		return XTC_E_AGAIN;           /* the log was rewritten under us */
	end = (off_t)atomic_load_explicit(&k->w->durable_off, memory_order_acquire);
	rc = wal_ock_copy(k, end, (off_t)(64u << 20));   /* bounded slice */
	wal_cmp_flush(&k->c);
	if (left != NULL)
		*left = (uint64_t)atomic_load_explicit(&k->w->durable_off,
		    memory_order_acquire) - (uint64_t)k->src_off;
	return rc != XTC_OK ? rc : (k->c.err ? XTC_E_INTERNAL : XTC_OK);
}

int
wal_ockpt_finish(wal_ock_t *k)
{
	wal_t *w = k->w;
	int rc;
	if (atomic_load_explicit(&w->gen, memory_order_acquire) != k->gen) {
		wal_ockpt_abort(k);
		return XTC_E_AGAIN;
	}
	if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
		wal_ockpt_abort(k);
		return XTC_E_IO;
	}
	/* Quiesced: the durable end is the append end, so after this copy
	 * the new file holds every record the old one does past the dump. */
	rc = wal_ock_copy(k, (off_t)atomic_load_explicit(&w->durable_off,
	    memory_order_acquire), 0);
	wal_cmp_flush(&k->c);
	if (rc != XTC_OK || k->c.err || k->c.off == 0 || fsync(k->c.fd) != 0) {
		wal_ockpt_abort(k);
		return rc != XTC_OK ? rc : XTC_E_INTERNAL;
	}
	(void)close(k->c.fd);
	k->c.fd = -1;
	if (rename(k->tmp, k->path) != 0) {
		wal_ockpt_abort(k);
		return XTC_E_INTERNAL;
	}
	wal_sync_dir(k->path);
	rc = wal_rebind_at(w, k->path, k->c.off, k->c.lsn);
	k->tmp[0] = '\0';                /* renamed: nothing to unlink */
	wal_ockpt_abort(k);              /* release the rest */
	return rc;
}

void
wal_ockpt_abort(wal_ock_t *k)
{
	if (k == NULL)
		return;
	if (k->c.fd >= 0)
		(void)close(k->c.fd);
	if (k->src_fd >= 0)
		(void)close(k->src_fd);
	if (k->tmp[0] != '\0')
		(void)unlink(k->tmp);
	free(k->c.buf);
	free(k->rbuf);
	free(k);
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
	atomic_store_explicit(&w->durable_off, 0, memory_order_release);
	atomic_fetch_add_explicit(&w->gen, 1, memory_order_acq_rel);
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

uint64_t
wal_size(const wal_t *w)
{
	return w == NULL ? 0 : (uint64_t)w->off;
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
	while ((rc = xtc_send(w->writer_pid, &kind, sizeof kind)) == XTC_E_AGAIN)
		(void)xtc_proc_sleep(10LL * 1000);   /* soft cap: never drop STOP */
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
	out->failed = w->s_failed;
}
