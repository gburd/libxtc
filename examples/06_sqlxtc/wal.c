/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/wal.c
 *	Write-ahead log with a group-commit writer process.  See wal.h
 *	and docs/M_SQLXTC_SCALEOUT.md (stage 1).
 *
 *	The writer owns the file; committers send records and park on an
 *	ack.  A batch is drained from the mailbox -- the first record
 *	blocks, then more are gathered with a TIMED receive until the
 *	window closes or the batch cap is hit -- and committed with one
 *	write(2) + one fdatasync(2), both offloaded via xtc_blocking_run
 *	so the loop keeps running while the writer parks on disk.
 *
 *	Dogfood note: this is implemented on raw xtc_proc send/recv, not
 *	xtc_svr, because group commit is the gen_server:reply/2 pattern
 *	(stash the requester, reply later, reply to many) and xtc_svr has
 *	no deferred reply -- its xtc_svr_call_t is stack-scoped to one
 *	handle_call.  See docs/M_SQLXTC_XTC_GAPS.md.
 */

#include "wal.h"

#include "xtc_blocking.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Wire kinds for a message to the writer. */
#define WAL_KIND_COMMIT  'C'
#define WAL_KIND_STOP    'S'

#define WAL_REC_HDR      10u     /* on-disk per-record: u64 lsn + u16 len */

struct wal_msg {                 /* committer -> writer */
	uint8_t   kind;
	xtc_pid_t reply_to;
	uint64_t  txn_id;
	uint16_t  len;
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

	/* Batch state -- touched only by the writer process. */
	uint8_t           *bbuf;
	size_t             bcap;
	size_t             blen;
	struct wal_pending *pend;
	uint32_t           pcount;
	uint64_t           next_lsn;
	uint64_t           durable_lsn;

	/* Stats. */
	uint64_t s_commits;
	uint64_t s_batches;
	uint64_t s_bytes;
	uint64_t s_maxbatch;
};

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- offloaded write + fsync ---- */
struct flush_io { int fd; const uint8_t *buf; size_t len; off_t off; };

static int
flush_io_fn(void *arg)
{
	struct flush_io *f = arg;
	size_t done = 0;

	while (done < f->len) {
		ssize_t w = pwrite(f->fd, f->buf + done, f->len - done,
		    f->off + (off_t)done);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return errno ? errno : -1;
		}
		done += (size_t)w;
	}
	if (fdatasync(f->fd) != 0)
		return errno ? errno : -1;
	return 0;
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
	w->fd = open(opts->path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (w->fd < 0) {
		free(w);
		return XTC_E_INTERNAL;   /* no XTC_E_IO in the core enum -- see gaps ledger */
	}
	w->off = 0;
	w->window_ns = opts->window_ns > 0 ? opts->window_ns : 500000;   /* 0.5ms */
	w->max_batch = opts->max_batch > 0 ? opts->max_batch : 256;
	w->bcap = 64 * 1024;
	w->bbuf = malloc(w->bcap);
	w->pend = calloc(w->max_batch, sizeof *w->pend);
	if (w->bbuf == NULL || w->pend == NULL) {
		free(w->bbuf); free(w->pend); close(w->fd); free(w);
		return XTC_E_NOMEM;
	}
	*out = w;
	return XTC_OK;
}

void
wal_close(wal_t *w)
{
	if (w == NULL)
		return;
	if (w->fd >= 0)
		close(w->fd);
	free(w->bbuf);
	free(w->pend);
	free(w);
}

/* Append one record to the current batch buffer (writer proc only). */
static int
batch_add(wal_t *w, const uint8_t *data, uint16_t len,
    xtc_pid_t reply_to, uint64_t txn_id)
{
	size_t need = WAL_REC_HDR + len;
	uint64_t lsn;

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
	memcpy(w->bbuf + w->blen + 8, &len, 2);
	memcpy(w->bbuf + w->blen + WAL_REC_HDR, data, len);
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
	struct flush_io f;
	struct wal_ack ack;
	int rc = 0;
	uint32_t i;

	if (w->pcount == 0)
		return;

	f.fd = w->fd; f.buf = w->bbuf; f.len = w->blen; f.off = w->off;
	if (xtc_blocking_run(flush_io_fn, &f, &rc) != XTC_OK)
		rc = flush_io_fn(&f);        /* off a loop: synchronous */
	(void)rc;                            /* a real engine would surface I/O errors */

	w->off += (off_t)w->blen;
	w->durable_lsn = w->pend[w->pcount - 1].lsn;
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
		int64_t deadline;
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

		/* Gather more until the window closes or the cap is hit. */
		deadline = now_ns() + w->window_ns;
		while (w->pcount < w->max_batch) {
			int64_t rem = deadline - now_ns();
			if (rem <= 0)
				break;
			m = NULL; n = 0;
			if (xtc_recv(&m, &n, rem) != XTC_OK || m == NULL)
				break;               /* timeout: close the batch */
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
	rc = xtc_proc_spawn(loop, wal_writer_proc, w, &o, &p);
	if (rc != XTC_OK)
		return rc;
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
wal_commit(wal_t *w, const void *record, uint16_t len, uint64_t *lsn)
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
wal_writer_stop(wal_t *w)
{
	uint8_t kind = WAL_KIND_STOP;

	if (w == NULL)
		return XTC_E_INVAL;
	return xtc_send(w->writer_pid, &kind, sizeof kind);
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
	out->durable_lsn = w->durable_lsn;
}
