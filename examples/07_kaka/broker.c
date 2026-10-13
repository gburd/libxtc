/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/broker.c
 *
 *	The broker's process kinds and the partition registry:
 *
 *	  partition_proc -- one per (topic, partition).  Owns a plog.
 *	    Drains its mailbox in batches: every PRODUCE in a batch is
 *	    appended, then ONE plog_sync makes the whole batch durable, then
 *	    every producer in it is acked (group commit).  A FETCH reads at
 *	    most FETCH_SLICE bytes per turn and re-queues the rest of the
 *	    request to itself, so a large cold read never holds the
 *	    partition (or, since reads go through xtc_aio, the loop).
 *
 *	  conn_proc -- one per client TCP connection.  Parks on the socket
 *	    with xtc_proc_wait_fd, reads a frame, sends a request to the
 *	    owning partition (or the group coordinator) and writes the reply.
 *
 *	A request message carries its own copy of the PRODUCE payload, so a
 *	connection that times out, or exits, never leaves a partition
 *	holding a pointer into a freed buffer.  Replies carry the request
 *	tag; a late reply to an abandoned request is discarded.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_async.h"
#include "xtc_credit.h"

#include "broker.h"
#include "frame.h"
#include "group.h"
#include "partition.h"
#include "metrics.h"

/* ---- conn <-> partition messages ---- */

enum { REQ_PRODUCE = 1, REQ_FETCH = 2, REQ_SHUTDOWN = 3, REQ_FETCH_CONT = 4 };

#define PART_BATCH       64                 /* requests per group commit */
#define FETCH_SLICE      (1u * 1024u * 1024u)
#define FETCH_DEFAULT    65536u
#define FETCH_MAX        (KAKA_MAX_FRAME - 1 - 12)   /* body after base+n */

/* Request header; a PRODUCE's records (wire encoded, n_records of them)
 * follow it in the same message. */
struct part_req {
	uint8_t    op;
	xtc_pid_t  reply;
	uint32_t   tag;
	uint32_t   n_records;   /* PRODUCE */
	uint64_t   offset;      /* FETCH */
	uint32_t   max_bytes;   /* FETCH */
	/* REQ_FETCH_CONT (sent by the partition to itself) */
	uint8_t   *buf;
	size_t     used;
	uint32_t   n;
	uint64_t   first;
	int64_t    t0;
};

enum { REP_OK = 0, REP_ERR = 1 };

struct part_reply {
	uint32_t   tag;
	int        ok;          /* 1 = success */
	uint16_t   err;         /* KAKA_ERR_* when !ok */
	uint32_t   n_stored;    /* PRODUCE: records stored */
	uint64_t   base_offset; /* PRODUCE: offset of the first record */
	uint8_t   *records;     /* FETCH: malloc'd RECORDS frame (conn frees) */
	size_t     records_len;
	uint64_t   hwm;
};

/* ---- configuration + registry ---- */

#define MAX_PARTITIONS 256

struct part_entry {
	char       topic[64];
	uint32_t   partition;
	xtc_pid_t  pid;
	int        in_use;
	_Atomic int done;          /* partition proc returned */
};

struct part_arg {
	char     topic[64];
	uint32_t partition;
	struct part_entry *ent;    /* NULL in the self-tests */
};

static struct part_entry  g_parts[MAX_PARTITIONS];
static pthread_mutex_t     g_parts_lock = PTHREAD_MUTEX_INITIALIZER;
static xtc_loop_t         *g_loop;
static char                g_log_dir[256];   /* empty = in-memory */
static int64_t             g_fsync_ns;       /* 0 = sync before ack */
static size_t              g_seg_bytes;
static uint64_t            g_retention;
static int64_t             g_req_timeout = 30LL * 1000 * 1000 * 1000;
static xtc_pid_t           g_coord;
static int                 g_have_coord;
static _Atomic int         g_stopping;       /* no new requests */

/* Connection table (for shutdown). */
static xtc_pid_t          *g_conns;
static int                *g_conn_fds;
static int                 g_max_conns;
static _Atomic int         g_n_conns;
static pthread_mutex_t     g_conns_lock = PTHREAD_MUTEX_INITIALIZER;

void broker_set_loop(xtc_loop_t *loop) { g_loop = loop; }
void broker_set_fsync_interval(int64_t ns) { g_fsync_ns = ns > 0 ? ns : 0; }
void broker_set_segment_bytes(size_t bytes) { g_seg_bytes = bytes; }
void broker_set_retention_bytes(uint64_t bytes) { g_retention = bytes; }

void
broker_set_request_timeout(int64_t ns)
{
	if (ns > 0)
		g_req_timeout = ns;
}

void
broker_set_log_dir(const char *dir)
{
	if (dir != NULL && dir[0] != '\0')
		snprintf(g_log_dir, sizeof g_log_dir, "%s", dir);
	else
		g_log_dir[0] = '\0';
}

/* ---- partition proc ---- */

struct pstate {
	plog_t     *log;
	int64_t     mem_charged;   /* budget charged for RAM-resident bytes */
	int64_t     last_sync;
	struct part_arg *pa;
};

/* Keep the memory budget equal to the bytes this partition holds in RAM
 * (eviction gives budget back). */
static void
__budget_settle(struct pstate *ps)
{
	int64_t now = (int64_t)plog_mem_bytes(ps->log);
	if (now < ps->mem_charged)
		kaka_mem_release(ps->mem_charged - now);
	else if (now > ps->mem_charged &&
	    kaka_mem_acquire(now - ps->mem_charged) != 0)
		now = ps->mem_charged;
	ps->mem_charged = now;
}

static void
__reply_err(xtc_pid_t to, uint32_t tag, uint16_t err)
{
	struct part_reply rep;
	memset(&rep, 0, sizeof rep);
	rep.tag = tag;
	rep.err = err;
	(void)xtc_send(to, &rep, sizeof rep);
}

/* Append one PRODUCE.  All-or-nothing against the memory budget.  Fills
 * rep (ok/err/base/n_stored); the ack is sent after the batch sync. */
static void
__do_produce(struct pstate *ps, const struct part_req *req,
    const uint8_t *recs, size_t recs_len, struct part_reply *rep)
{
	kaka_produce_t pr;
	kaka_record_t r;
	int64_t need = 0, t0 = xtc_clock_mono();
	uint32_t i;
	int got;

	memset(&pr, 0, sizeof pr);
	pr.n_records = req->n_records;
	pr.records_cur = recs;
	pr.records_end = recs + recs_len;
	while ((got = kaka_produce_next_record(&pr, &r)) == 1)
		need += (int64_t)r.key_len + r.value_len;
	if (got < 0) { rep->err = KAKA_ERR_BAD_REQUEST; return; }
	if (plog_failed(ps->log)) { rep->err = KAKA_ERR_STORAGE; return; }
	if (need > 0 && kaka_mem_acquire(need) != 0) {
		/* Over budget.  With persistence the RAM window is only a
		 * cache: drop it and retry before refusing. */
		plog_evict_all(ps->log);
		__budget_settle(ps);
		if (kaka_mem_acquire(need) != 0) {
			rep->err = KAKA_ERR_MEMORY;
			kaka_metrics_produce(0, 0, -1, req->n_records);
			return;
		}
	}
	ps->mem_charged += need;
	pr.records_cur = recs;
	for (i = 0; kaka_produce_next_record(&pr, &r) == 1; i++) {
		int64_t off = plog_append(ps->log, &r);
		if (off < 0) {
			rep->err = plog_failed(ps->log) ? KAKA_ERR_STORAGE :
			    KAKA_ERR_MEMORY;
			break;
		}
		if (i == 0) rep->base_offset = (uint64_t)off;
	}
	__budget_settle(ps);
	rep->n_stored = i;
	rep->ok = (rep->err == 0);
	rep->hwm = plog_high_water(ps->log);
	kaka_metrics_produce(i, (size_t)need, xtc_clock_mono() - t0,
	    rep->ok ? 0 : req->n_records - i);
}

/* One FETCH slice; finishes (replies) or re-queues itself. */
static void
__do_fetch(struct pstate *ps, struct part_req *req)
{
	/* Consumers see acked records only: synced ones in durable mode. */
	uint64_t end = g_fsync_ns == 0 ? plog_synced(ps->log) :
	    plog_high_water(ps->log);
	uint8_t *buf = req->buf;
	struct part_reply rep;
	size_t cap = (size_t)req->max_bytes + 17;
	uint32_t n = 0;
	size_t need = 0;
	long w;

	if (buf == NULL) {
		req->first = req->offset < plog_start(ps->log) ?
		    plog_start(ps->log) : req->offset;
		req->offset = req->first;
		req->t0 = xtc_clock_mono();
		req->used = 17;
		if ((buf = malloc(cap)) == NULL) {
			__reply_err(req->reply, req->tag, KAKA_ERR_MEMORY);
			return;
		}
		req->buf = buf;
	}
	if (req->offset < end) {
		size_t room = cap - req->used;
		if (room > FETCH_SLICE) room = FETCH_SLICE;
		w = plog_fetch(ps->log, &req->offset, end, buf + req->used, room,
		    &n, &need);
		if (w < 0) {
			free(buf);
			__reply_err(req->reply, req->tag, KAKA_ERR_STORAGE);
			return;
		}
		req->used += (size_t)w;
		req->n += n;
		/* A single record larger than the slice still goes out whole
		 * when it fits the request. */
		if (n == 0 && need > 0 && need <= cap - req->used) {
			w = plog_fetch(ps->log, &req->offset, end,
			    buf + req->used, need, &n, NULL);
			if (w < 0) {
				free(buf);
				__reply_err(req->reply, req->tag,
				    KAKA_ERR_STORAGE);
				return;
			}
			req->used += (size_t)w;
			req->n += n;
		}
		if (n > 0 && req->offset < end && req->used < cap &&
		    !atomic_load(&g_stopping)) {
			req->op = REQ_FETCH_CONT;
			if (xtc_send(xtc_self(), req, sizeof *req) == XTC_OK) {
				xtc_yield();     /* let the loop run others */
				return;          /* continue next turn */
			}
		}
	}
	(void)kaka_encode_records_header(buf, cap, req->first, req->n);
	(void)kaka_frame_header(buf, cap, KAKA_RECORDS, req->used - 5);
	memset(&rep, 0, sizeof rep);
	rep.tag = req->tag;
	rep.ok = 1;
	rep.records = buf;
	rep.records_len = req->used;
	rep.hwm = end;
	if (xtc_send(req->reply, &rep, sizeof rep) != XTC_OK)
		free(buf);
	kaka_metrics_fetch(req->n, req->used, xtc_clock_mono() - req->t0);
}

/* Make the batch durable (or not, in relaxed mode) and send the acks. */
static void
__commit_batch(struct pstate *ps, struct part_reply *reps, xtc_pid_t *to,
    int n)
{
	int i, sync_ok = 1, any = 0;
	int64_t now = xtc_clock_mono();

	for (i = 0; i < n; i++)
		any |= reps[i].n_stored > 0;
	if (any && (g_fsync_ns == 0 || now - ps->last_sync >= g_fsync_ns)) {
		sync_ok = plog_sync(ps->log) == 0;
		ps->last_sync = now;
		if (!sync_ok)
			XTC_LOG_ERROR_F("partition %s/%u: fsync failed; "
			    "partition is now read-only", ps->pa->topic,
			    ps->pa->partition);
	}
	for (i = 0; i < n; i++) {
		if (!sync_ok && g_fsync_ns == 0 && reps[i].n_stored > 0) {
			reps[i].ok = 0;
			reps[i].err = KAKA_ERR_STORAGE;
		}
		(void)xtc_send(to[i], &reps[i], sizeof reps[i]);
	}
}

static int
__open_log(struct part_arg *pa, plog_t **log)
{
	char pdir[512];
	if (g_log_dir[0] == '\0')
		return plog_create(log);
	snprintf(pdir, sizeof pdir, "%s/%s-%u", g_log_dir, pa->topic,
	    pa->partition);
	(void)mkdir(g_log_dir, 0755);
	(void)mkdir(pdir, 0755);
	if (plog_create_ex(pdir, g_seg_bytes, log) != 0)
		return -1;
	plog_set_retention(*log, g_retention);
	return 0;
}

static void
partition_proc(void *arg)
{
	struct pstate ps;
	struct part_reply reps[PART_BATCH];
	xtc_pid_t to[PART_BATCH];
	xtc_pid_t shut_to = XTC_PID_NONE;
	uint32_t shut_tag = 0;
	int stop = 0;
	void *qmsg;
	size_t qlen;

	memset(&ps, 0, sizeof ps);
	ps.pa = arg;
	if (__open_log(ps.pa, &ps.log) != 0) {
		XTC_LOG_ERROR_F("partition %s/%u: log open failed",
		    ps.pa->topic, ps.pa->partition);
		ps.log = NULL;
	} else {
		XTC_LOG_INFO_F("partition %s/%u up (hw %llu)", ps.pa->topic,
		    ps.pa->partition,
		    (unsigned long long)plog_high_water(ps.log));
	}
	ps.last_sync = xtc_clock_mono();

	while (!stop) {
		int nb = 0;
		int64_t wait = -1;
		void *msg;
		size_t mlen;

		if (g_fsync_ns > 0 && ps.log != NULL &&
		    plog_synced(ps.log) != plog_high_water(ps.log))
			wait = g_fsync_ns;
		if (xtc_recv(&msg, &mlen, wait) != XTC_OK) {
			if (ps.log != NULL && plog_sync(ps.log) == 0)
				ps.last_sync = xtc_clock_mono();
			continue;
		}
		/* Drain up to PART_BATCH requests, then group-commit. */
		for (;;) {
			struct part_req req;
			if (msg != NULL && mlen >= sizeof req) {
				memcpy(&req, msg, sizeof req);
				if (req.op == REQ_SHUTDOWN) {
					stop = 1;
					shut_to = req.reply;
					shut_tag = req.tag;
				} else if (ps.log == NULL) {
					free(req.op == REQ_FETCH_CONT ?
					    req.buf : NULL);
					__reply_err(req.reply, req.tag,
					    KAKA_ERR_STORAGE);
				} else if (req.op == REQ_PRODUCE) {
					memset(&reps[nb], 0, sizeof reps[nb]);
					reps[nb].tag = req.tag;
					__do_produce(&ps, &req,
					    (const uint8_t *)msg + sizeof req,
					    mlen - sizeof req, &reps[nb]);
					to[nb++] = req.reply;
				} else if (req.op == REQ_FETCH) {
					req.buf = NULL;
					req.n = 0;
					if (req.max_bytes == 0)
						req.max_bytes = FETCH_DEFAULT;
					if (req.max_bytes > FETCH_MAX)
						req.max_bytes = FETCH_MAX;
					__do_fetch(&ps, &req);
				} else if (req.op == REQ_FETCH_CONT) {
					__do_fetch(&ps, &req);
				}
			}
			if (msg != NULL) xtc_free(msg);
			if (stop || nb == PART_BATCH ||
			    xtc_recv(&msg, &mlen, 0) != XTC_OK)
				break;
		}
		if (nb > 0)
			__commit_batch(&ps, reps, to, nb);
	}

	/* Answer whatever is still queued, then close the log durably. */
	while (xtc_recv(&qmsg, &qlen, 0) == XTC_OK) {
		struct part_req req;
		if (qmsg != NULL && qlen >= sizeof req) {
			memcpy(&req, qmsg, sizeof req);
			if (req.op == REQ_FETCH_CONT)
				free(req.buf);
			if (req.op != REQ_SHUTDOWN)
				__reply_err(req.reply, req.tag,
				    KAKA_ERR_SHUTDOWN);
		}
		if (qmsg != NULL) xtc_free(qmsg);
	}
	if (ps.log != NULL) {
		if (plog_sync(ps.log) != 0)
			XTC_LOG_ERROR_F("partition %s/%u: final fsync failed",
			    ps.pa->topic, ps.pa->partition);
		plog_destroy(ps.log);
	}
	kaka_mem_release(ps.mem_charged);
	if (!xtc_pid_is_none(shut_to)) {
		memset(&reps[0], 0, sizeof reps[0]);
		reps[0].tag = shut_tag;
		reps[0].ok = 1;
		(void)xtc_send(shut_to, &reps[0], sizeof reps[0]);
	}
	if (ps.pa->ent != NULL)
		atomic_store(&ps.pa->ent->done, 1);
	free(ps.pa);
}

/* Resolve (or lazily spawn) the partition proc for topic/partition. */
static int
partition_for(const char *topic, uint16_t topic_len, uint32_t partition,
              xtc_pid_t *out)
{
	int i, free_slot = -1, rc = -1;
	char tkey[64];
	struct part_arg *pa;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;

	if (topic_len == 0 || topic_len >= sizeof tkey)
		return -1;
	for (i = 0; i < topic_len; i++)       /* topic names a directory */
		if (!((topic[i] >= 'a' && topic[i] <= 'z') ||
		    (topic[i] >= 'A' && topic[i] <= 'Z') ||
		    (topic[i] >= '0' && topic[i] <= '9') ||
		    topic[i] == '_' || topic[i] == '-' ||
		    (topic[i] == '.' && i > 0)))
			return -1;
	memcpy(tkey, topic, topic_len);
	tkey[topic_len] = '\0';

	(void)pthread_mutex_lock(&g_parts_lock);
	for (i = 0; i < MAX_PARTITIONS; i++) {
		if (g_parts[i].in_use) {
			if (g_parts[i].partition == partition &&
			    strcmp(g_parts[i].topic, tkey) == 0) {
				*out = g_parts[i].pid;
				rc = 0;
				goto out;
			}
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot < 0 || atomic_load(&g_stopping) ||
	    (pa = calloc(1, sizeof *pa)) == NULL)
		goto out;
	memcpy(pa->topic, tkey, (size_t)topic_len + 1);
	pa->partition = partition;
	pa->ent = &g_parts[free_slot];
	atomic_store(&pa->ent->done, 0);
	opts.name = "kaka-partition";
	opts.mailbox_cap = 1u << 16;
	if (xtc_proc_spawn(g_loop, partition_proc, pa, &opts, &pid) != XTC_OK) {
		free(pa);
		goto out;
	}
	g_parts[free_slot].in_use = 1;
	g_parts[free_slot].partition = partition;
	memcpy(g_parts[free_slot].topic, tkey, (size_t)topic_len + 1);
	g_parts[free_slot].pid = pid;
	*out = pid;
	rc = 0;
out:
	(void)pthread_mutex_unlock(&g_parts_lock);
	return rc;
}

/* ---- connection proc ---- */

struct conn_arg { int fd; int slot; };

/* Read one complete frame into *buf.  Returns the frame length, 0 on
 * EOF or when shutdown stops the connection between requests, -1 on
 * error. */
static long
conn_read_frame(int fd, uint8_t **buf, size_t *cap, size_t *have)
{
	for (;;) {
		uint8_t type;
		const uint8_t *body;
		size_t body_len;
		ssize_t r;
		uint32_t revents = 0;
		long n = kaka_frame_parse(*buf, *have, &type, &body, &body_len);
		if (n > 0) return n;
		if (n < 0) return -1;
		if (atomic_load(&g_stopping)) return 0;
		if (*have == *cap) {
			size_t ncap = *cap ? *cap * 2 : 4096;
			uint8_t *nb = realloc(*buf, ncap);
			if (nb == NULL) return -1;
			*buf = nb; *cap = ncap;
		}
		r = recv(fd, (char *)(*buf + *have), *cap - *have, 0);
		if (r > 0) { *have += (size_t)r; continue; }
		if (r == 0) return 0;
		if (errno == EINTR) continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
		/* broker_shutdown wakes us (xtc_proc_wake); the 1 s timeout
		 * is only a backstop for the shutdown-flag check. */
		if (xtc_proc_wait_fd(fd, XTC_IO_READABLE, 1000LL * 1000 * 1000,
		    &revents) != XTC_OK && !(revents & XTC_WAIT_TIMEOUT))
			return -1;
		if (revents & XTC_WAIT_MAILBOX) {   /* stale replies */
			void *m;
			size_t ml;
			while (xtc_recv(&m, &ml, 0) == XTC_OK) {
				struct part_reply rep;
				if (m != NULL && ml == sizeof rep) {
					memcpy(&rep, m, sizeof rep);
					free(rep.records);
				}
				xtc_free(m);
			}
		}
	}
}

static int
conn_write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t w = send(fd, (const char *)(buf + off), len - off,
		    MSG_NOSIGNAL);
		if (w > 0) { off += (size_t)w; continue; }
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			uint32_t revents = 0;
			if (xtc_proc_wait_fd(fd, XTC_IO_WRITABLE,
			    g_req_timeout, &revents) != XTC_OK)
				return -1;
			continue;
		}
		if (w < 0 && errno == EINTR) continue;
		return -1;
	}
	return 0;
}

/* Wait for the reply carrying `tag`; replies to earlier (abandoned)
 * requests are discarded.  Returns 0 and fills *out, or -1 on timeout. */
static int
await_reply(uint32_t tag, void *out, size_t out_size)
{
	int64_t deadline = xtc_clock_mono() + g_req_timeout;
	for (;;) {
		void *m;
		size_t ml;
		uint32_t got;
		int64_t left = deadline - xtc_clock_mono();
		if (left <= 0) return -1;
		if (xtc_recv(&m, &ml, left) != XTC_OK)
			return -1;
		if (m == NULL) continue;
		if (ml == out_size) {
			memcpy(&got, m, sizeof got);
			if (got == tag) {
				memcpy(out, m, out_size);
				xtc_free(m);
				return 0;
			}
			if (ml == sizeof(struct part_reply)) {
				struct part_reply stale;
				memcpy(&stale, m, sizeof stale);
				free(stale.records);
			}
		}
		xtc_free(m);
	}
}

static int
conn_send_error(int fd, uint16_t code, const char *msg)
{
	uint8_t err[128];
	long e = kaka_encode_error(err, sizeof err, code, msg);
	return e > 0 ? conn_write_all(fd, err, (size_t)e) : -1;
}

static const char *
__err_text(uint16_t e)
{
	switch (e) {
	case KAKA_ERR_MEMORY:   return "memory budget exceeded; nothing stored";
	case KAKA_ERR_STORAGE:  return "storage failure; partition read-only";
	case KAKA_ERR_TIMEOUT:  return "request timed out";
	case KAKA_ERR_SHUTDOWN: return "broker shutting down";
	case KAKA_ERR_GROUP:    return "offset store failure";
	default:                return "bad request";
	}
}

/* Handle one PRODUCE/FETCH.  Returns 0 to keep the connection, -1 to
 * close it. */
static int
conn_partition_req(int fd, uint8_t type, const uint8_t *body,
    size_t body_len, uint32_t tag)
{
	struct part_req req;
	struct part_reply rep;
	const char *topic;
	uint16_t topic_len;
	uint32_t partition;
	xtc_pid_t ppid;
	uint8_t *msg = NULL;
	size_t msg_len = sizeof req;
	int rc;

	memset(&req, 0, sizeof req);
	req.reply = xtc_self();
	req.tag = tag;
	if (type == KAKA_PRODUCE) {
		kaka_produce_t p;
		if (kaka_decode_produce(body, body_len, &p) != 0)
			return conn_send_error(fd, KAKA_ERR_BAD_REQUEST,
			    "bad request"), -1;
		req.op = REQ_PRODUCE;
		req.n_records = p.n_records;
		topic = p.topic; topic_len = p.topic_len;
		partition = p.partition;
		msg_len += (size_t)(p.records_end - p.records_cur);
		if ((msg = malloc(msg_len)) == NULL)
			return conn_send_error(fd, KAKA_ERR_MEMORY,
			    __err_text(KAKA_ERR_MEMORY));
		memcpy(msg + sizeof req, p.records_cur,
		    (size_t)(p.records_end - p.records_cur));
	} else {
		kaka_fetch_t f;
		if (kaka_decode_fetch(body, body_len, &f) != 0)
			return conn_send_error(fd, KAKA_ERR_BAD_REQUEST,
			    "bad request"), -1;
		req.op = REQ_FETCH;
		req.offset = f.offset;
		req.max_bytes = f.max_bytes;
		topic = f.topic; topic_len = f.topic_len;
		partition = f.partition;
		msg = (uint8_t *)&req;
	}
	if (partition_for(topic, topic_len, partition, &ppid) != 0) {
		if (msg != (uint8_t *)&req) free(msg);
		return conn_send_error(fd, atomic_load(&g_stopping) ?
		    KAKA_ERR_SHUTDOWN : KAKA_ERR_BAD_REQUEST,
		    atomic_load(&g_stopping) ? "broker shutting down" :
		    "bad topic or too many partitions");
	}
	if (msg != (uint8_t *)&req)
		memcpy(msg, &req, sizeof req);
	rc = xtc_send(ppid, msg, msg_len);
	if (msg != (uint8_t *)&req) free(msg);
	if (rc != XTC_OK)
		return conn_send_error(fd, KAKA_ERR_TIMEOUT, "partition busy");
	if (await_reply(tag, &rep, sizeof rep) != 0)
		return conn_send_error(fd, KAKA_ERR_TIMEOUT,
		    __err_text(KAKA_ERR_TIMEOUT));
	if (!rep.ok) {
		free(rep.records);
		return conn_send_error(fd, rep.err, __err_text(rep.err));
	}
	if (type == KAKA_PRODUCE) {
		uint8_t ack[13];
		(void)kaka_encode_produce_ack(ack, sizeof ack, rep.base_offset);
		return conn_write_all(fd, ack, sizeof ack);
	}
	rc = conn_write_all(fd, rep.records, rep.records_len);
	free(rep.records);
	return rc;
}

static int
conn_group_req(int fd, uint8_t type, const uint8_t *body, size_t body_len,
    uint32_t tag)
{
	kaka_commit_t c;
	struct grp_req req;
	struct grp_reply rep;
	uint8_t out[16];
	long n;
	int dec = type == KAKA_COMMIT ? kaka_decode_commit(body, body_len, &c) :
	    kaka_decode_offsets(body, body_len, &c);

	if (dec != 0 || c.group_len == 0 || c.group_len >= KAKA_GROUP_NAME_MAX ||
	    c.topic_len == 0 || c.topic_len >= KAKA_TOPIC_NAME_MAX)
		return conn_send_error(fd, KAKA_ERR_BAD_REQUEST, "bad request"), -1;
	if (!g_have_coord)
		return conn_send_error(fd, KAKA_ERR_GROUP, "no offset store");
	memset(&req, 0, sizeof req);
	req.op = type == KAKA_COMMIT ? GRP_COMMIT : GRP_FETCH;
	req.reply = xtc_self();
	req.tag = tag;
	memcpy(req.group, c.group, c.group_len);
	memcpy(req.topic, c.topic, c.topic_len);
	req.partition = c.partition;
	req.offset = c.offset;
	if (xtc_send(g_coord, &req, sizeof req) != XTC_OK ||
	    await_reply(tag, &rep, sizeof rep) != 0)
		return conn_send_error(fd, KAKA_ERR_TIMEOUT,
		    __err_text(KAKA_ERR_TIMEOUT));
	if (!rep.ok)
		return conn_send_error(fd, KAKA_ERR_GROUP,
		    __err_text(KAKA_ERR_GROUP));
	n = type == KAKA_COMMIT ? kaka_encode_commit_ack(out, sizeof out) :
	    kaka_encode_offset(out, sizeof out, rep.found, rep.offset);
	return conn_write_all(fd, out, (size_t)n);
}

static void
conn_proc(void *arg)
{
	struct conn_arg *ca = arg;
	int fd = ca->fd, slot = ca->slot;
	uint8_t *buf = NULL;
	size_t cap = 0, have = 0;
	uint32_t tag = 0;
	free(ca);

	for (;;) {
		uint8_t type;
		const uint8_t *body;
		size_t body_len;
		int rc;
		long fn = conn_read_frame(fd, &buf, &cap, &have);
		if (fn <= 0) break;
		(void)kaka_frame_parse(buf, have, &type, &body, &body_len);
		tag++;
		if (type == KAKA_PRODUCE || type == KAKA_FETCH)
			rc = conn_partition_req(fd, type, body, body_len, tag);
		else if (type == KAKA_COMMIT || type == KAKA_OFFSETS)
			rc = conn_group_req(fd, type, body, body_len, tag);
		else
			rc = (conn_send_error(fd, KAKA_ERR_BAD_REQUEST,
			    "bad request"), -1);
		if (rc != 0) break;
		memmove(buf, buf + fn, have - (size_t)fn);
		have -= (size_t)fn;
	}

	free(buf);
	(void)pthread_mutex_lock(&g_conns_lock);
	g_conns[slot] = XTC_PID_NONE;
	g_conn_fds[slot] = -1;
	(void)pthread_mutex_unlock(&g_conns_lock);
	xtc_net_close(fd);
	atomic_fetch_sub(&g_n_conns, 1);
}

/* ---- startup / shutdown ---- */

int
broker_start(xtc_loop_t *loop, int max_conns)
{
	char odir[512];
	int i;

	g_loop = loop;
	if (max_conns < 1) max_conns = 1;
	g_conns = calloc((size_t)max_conns, sizeof *g_conns);
	g_conn_fds = calloc((size_t)max_conns, sizeof *g_conn_fds);
	if (g_conns == NULL || g_conn_fds == NULL) return -1;
	for (i = 0; i < max_conns; i++) {
		g_conns[i] = XTC_PID_NONE;
		g_conn_fds[i] = -1;
	}
	g_max_conns = max_conns;
	if (g_log_dir[0] != '\0') {
		(void)mkdir(g_log_dir, 0755);
		snprintf(odir, sizeof odir, "%s/__offsets", g_log_dir);
		(void)mkdir(odir, 0755);
	}
	if (group_coordinator_spawn_ex(loop, g_log_dir[0] ? odir : NULL,
	    &g_coord) != XTC_OK)
		return -1;
	g_have_coord = 1;
	return 0;
}

int
broker_spawn_conn(xtc_loop_t *loop, int fd)
{
	struct conn_arg *ca;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i, slot = -1;

	if (atomic_load(&g_stopping)) return -1;
	(void)pthread_mutex_lock(&g_conns_lock);
	for (i = 0; i < g_max_conns; i++)
		if (g_conn_fds[i] < 0) { slot = i; break; }
	if (slot >= 0) g_conn_fds[slot] = fd;
	(void)pthread_mutex_unlock(&g_conns_lock);
	if (slot < 0 || (ca = calloc(1, sizeof *ca)) == NULL)
		goto fail;
	ca->fd = fd;
	ca->slot = slot;
	opts.name = "kaka-conn";
	atomic_fetch_add(&g_n_conns, 1);
	(void)pthread_mutex_lock(&g_conns_lock);
	if (xtc_proc_spawn(loop, conn_proc, ca, &opts, &pid) != XTC_OK) {
		(void)pthread_mutex_unlock(&g_conns_lock);
		atomic_fetch_sub(&g_n_conns, 1);
		free(ca);
		goto fail;
	}
	if (g_conn_fds[slot] == fd) g_conns[slot] = pid;
	(void)pthread_mutex_unlock(&g_conns_lock);
	return 0;
fail:
	if (slot >= 0) {
		(void)pthread_mutex_lock(&g_conns_lock);
		g_conn_fds[slot] = -1;
		(void)pthread_mutex_unlock(&g_conns_lock);
	}
	return -1;
}

void
broker_shutdown(int64_t grace_ns)
{
	int64_t deadline = xtc_clock_mono() + grace_ns;
	struct part_req req;
	int i, pending;

	/* 1. Connections stop reading new requests, finish the one in
	 *    flight and close.  Wake the idle ones so they see the flag. */
	atomic_store(&g_stopping, 1);
	(void)pthread_mutex_lock(&g_conns_lock);
	for (i = 0; i < g_max_conns; i++)
		if (!xtc_pid_is_none(g_conns[i]))
			(void)xtc_proc_wake(g_conns[i]);
	(void)pthread_mutex_unlock(&g_conns_lock);
	while (atomic_load(&g_n_conns) > 0 && xtc_clock_mono() < deadline)
		(void)xtc_proc_sleep(10LL * 1000 * 1000);
	/* 2. Cut the stragglers (a client that never reads its reply). */
	if (atomic_load(&g_n_conns) > 0) {
		XTC_LOG_WARN_F("kaka: cutting %d connection(s)",
		    atomic_load(&g_n_conns));
		(void)pthread_mutex_lock(&g_conns_lock);
		for (i = 0; i < g_max_conns; i++)
			if (g_conn_fds[i] >= 0)
				(void)shutdown(g_conn_fds[i], SHUT_RDWR);
		(void)pthread_mutex_unlock(&g_conns_lock);
	}
	/* 3. Partitions answer what is queued, sync, close. */
	memset(&req, 0, sizeof req);
	req.op = REQ_SHUTDOWN;
	req.reply = xtc_self();
	(void)pthread_mutex_lock(&g_parts_lock);
	for (i = 0; i < MAX_PARTITIONS; i++)
		if (g_parts[i].in_use)
			(void)xtc_send(g_parts[i].pid, &req, sizeof req);
	(void)pthread_mutex_unlock(&g_parts_lock);
	deadline = xtc_clock_mono() + 10LL * 1000 * 1000 * 1000;
	for (;;) {
		void *m;
		size_t ml;
		pending = 0;
		for (i = 0; i < MAX_PARTITIONS; i++)
			if (g_parts[i].in_use && !atomic_load(&g_parts[i].done))
				pending++;
		if ((pending == 0 && atomic_load(&g_n_conns) == 0) ||
		    xtc_clock_mono() >= deadline)
			break;
		if (xtc_recv(&m, &ml, 10LL * 1000 * 1000) == XTC_OK)
			xtc_free(m);
	}
	if (pending > 0)
		XTC_LOG_ERROR_F("kaka: %d partition(s) did not close", pending);
	/* 4. The offset store. */
	if (g_have_coord) {
		struct grp_req g;
		struct grp_reply r;
		memset(&g, 0, sizeof g);
		g.op = GRP_SHUTDOWN;
		g.reply = xtc_self();
		g.tag = 0xfffffffu;
		if (xtc_send(g_coord, &g, sizeof g) == XTC_OK)
			(void)await_reply(g.tag, &r, sizeof r);
		g_have_coord = 0;
	}
}

/* ---- in-process self-tests (no socket, no daemon) ---- */

struct st {
	int        result;
	xtc_pid_t  part;
	const char *dir;
	int        a, b;          /* test parameters */
	uint64_t   u;             /* test output */
};

/* One wire record: key_len | value_len | key | value. */
static size_t
st_rec(uint8_t *p, const void *k, uint32_t kl, const void *v, uint32_t vl)
{
	kaka_put_u32(p, kl);
	kaka_put_u32(p + 4, vl);
	if (kl) memcpy(p + 8, k, kl);
	if (vl) memcpy(p + 8 + kl, v, vl);
	return 8 + (size_t)kl + vl;
}

static int
st_send_produce(xtc_pid_t part, uint32_t tag, const uint8_t *recs,
    size_t len, uint32_t n)
{
	struct part_req req;
	uint8_t *m = malloc(sizeof req + len);
	int rc;
	if (m == NULL) return -1;
	memset(&req, 0, sizeof req);
	req.op = REQ_PRODUCE;
	req.reply = xtc_self();
	req.tag = tag;
	req.n_records = n;
	memcpy(m, &req, sizeof req);
	if (len) memcpy(m + sizeof req, recs, len);
	rc = xtc_send(part, m, sizeof req + len);
	free(m);
	return rc == XTC_OK ? 0 : -1;
}

static int
st_produce(xtc_pid_t part, uint32_t tag, const void *v, uint32_t vl,
    struct part_reply *rep)
{
	uint8_t buf[2048];
	size_t n;
	if (vl > sizeof buf - 8) return -1;
	n = st_rec(buf, NULL, 0, v, vl);
	if (st_send_produce(part, tag, buf, n, 1) != 0) return -1;
	return await_reply(tag, rep, sizeof *rep);
}

static int
st_fetch(xtc_pid_t part, uint32_t tag, uint64_t off, uint32_t max,
    struct part_reply *rep)
{
	struct part_req req;
	memset(&req, 0, sizeof req);
	req.op = REQ_FETCH;
	req.reply = xtc_self();
	req.tag = tag;
	req.offset = off;
	req.max_bytes = max;
	if (xtc_send(part, &req, sizeof req) != XTC_OK) return -1;
	return await_reply(tag, rep, sizeof *rep);
}

/* Records in a FETCH reply; checks offsets are contiguous from `off`
 * and, if vlen > 0, that value i is the 4-byte big-endian (off + i). */
static long
st_count(const struct part_reply *rep, uint64_t off, int check)
{
	const uint8_t *p, *end;
	uint32_t n, i;
	if (rep->records == NULL || rep->records_len < 17) return -1;
	if (kaka_get_u32(rep->records) != rep->records_len - 4) return -1;
	n = kaka_get_u32(rep->records + 13);
	p = rep->records + 17;
	end = rep->records + rep->records_len;
	for (i = 0; i < n; i++) {
		uint32_t kl, vl;
		if ((size_t)(end - p) < 16) return -1;
		kl = kaka_get_u32(p + 8);
		vl = kaka_get_u32(p + 12);
		if (kaka_get_u64(p) != off + i) return -1;
		if ((size_t)(end - p) < 16 + (size_t)kl + vl) return -1;
		if (check && (vl < 4 ||
		    kaka_get_u32(p + 16 + kl) != (uint32_t)(off + i)))
			return -1;
		p += 16 + (size_t)kl + vl;
	}
	return p == end ? (long)n : -1;
}

static void
st_shutdown(xtc_pid_t part)
{
	struct part_req req;
	struct part_reply rep;
	memset(&req, 0, sizeof req);
	req.op = REQ_SHUTDOWN;
	req.reply = xtc_self();
	req.tag = 0xdead;
	if (xtc_send(part, &req, sizeof req) == XTC_OK)
		(void)await_reply(0xdead, &rep, sizeof rep);
}

static int
st_run(void (*client)(void *), struct st *st)
{
	xtc_loop_t *loop = NULL;
	struct part_arg *pa;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;

	if (xtc_loop_init(&loop) != XTC_OK) return -1;
	if ((pa = calloc(1, sizeof *pa)) == NULL) return -1;
	pa->topic[0] = 't';
	opts.name = "selftest-partition";
	opts.mailbox_cap = 1u << 16;
	if (xtc_proc_spawn(loop, partition_proc, pa, &opts, &st->part)
	    != XTC_OK) {
		free(pa);
		return -1;
	}
	opts.name = "selftest-client";
	if (xtc_proc_spawn(loop, client, st, &opts, &pid) != XTC_OK)
		return -1;
	if (xtc_loop_run(loop) != XTC_OK) return -1;
	(void)xtc_loop_fini(loop);
	return st->result;
}

/* Produce 3 records in one batch, fetch them back. */
static void
selftest_client(void *arg)
{
	struct st *st = arg;
	struct part_reply rep;
	uint8_t recs[64];
	size_t n = 0;

	n += st_rec(recs + n, "a", 1, "\0\0\0\0", 4);
	n += st_rec(recs + n, NULL, 0, "\0\0\0\1", 4);
	n += st_rec(recs + n, "c", 1, "\0\0\0\2", 4);
	if (st_send_produce(st->part, 1, recs, n, 3) != 0 ||
	    await_reply(1, &rep, sizeof rep) != 0) { st->result = 1; goto done; }
	if (!rep.ok || rep.base_offset != 0 || rep.n_stored != 3 ||
	    rep.hwm != 3) { st->result = 2; goto done; }
	if (st_fetch(st->part, 2, 0, 65536, &rep) != 0 || !rep.ok) {
		st->result = 3; goto done;
	}
	if (st_count(&rep, 0, 1) != 3) st->result = 4;
	free(rep.records);
done:
	st_shutdown(st->part);
}

int
broker_selftest(void)
{
	struct st st;
	memset(&st, 0, sizeof st);
	broker_set_log_dir(NULL);
	return st_run(selftest_client, &st);
}

/* Credit-based backpressure: at most CREDIT_BUDGET PRODUCEs in flight,
 * enforced by an xtc_credit window; every record lands in order. */
#define CREDIT_BUDGET   4
#define CREDIT_TOTAL    200

static void
credit_producer(void *arg)
{
	struct st *st = arg;
	xtc_credit_t *cw = NULL;
	int sent = 0, acked = 0;
	uint8_t rec[16];

	if (xtc_credit_create(CREDIT_BUDGET, &cw) != XTC_OK) {
		st->result = 5; return;
	}
	while (acked < CREDIT_TOTAL) {
		void *msg;
		size_t mlen;
		struct part_reply rep;
		uint8_t v[4];
		while (sent < CREDIT_TOTAL &&
		    xtc_credit_try_acquire(cw) == XTC_OK) {
			kaka_put_u32(v, (uint32_t)sent);
			if (st_send_produce(st->part, (uint32_t)sent + 1, rec,
			    st_rec(rec, NULL, 0, v, 4), 1) != 0) {
				st->result = 2; goto done;
			}
			sent++;
		}
		if (xtc_recv(&msg, &mlen, 2000LL * 1000000) != XTC_OK ||
		    msg == NULL) { st->result = 3; goto done; }
		memcpy(&rep, msg, sizeof rep);
		xtc_free(msg);
		if (!rep.ok || rep.base_offset != rep.tag - 1) {
			st->result = 4; goto done;
		}
		st->u = rep.hwm;
		(void)xtc_credit_release(cw);
		acked++;
	}
done:
	st->a = (int)xtc_credit_peak(cw);
	xtc_credit_destroy(cw);
	st_shutdown(st->part);
}

int
broker_credit_selftest(int *max_in_flight, uint64_t *hwm)
{
	struct st st;
	int rc;
	memset(&st, 0, sizeof st);
	broker_set_log_dir(NULL);
	rc = st_run(credit_producer, &st);
	if (max_in_flight) *max_in_flight = st.a;
	if (hwm) *hwm = st.u;
	return rc;
}

/* Observability + the memory budget: st->a single-record produces,
 * st->b = 1 to fetch afterwards.  st->u counts rejected produces, which
 * must each be answered with KAKA_ERR_MEMORY. */
static void
metrics_client(void *arg)
{
	struct st *st = arg;
	struct part_reply rep;
	uint8_t v[4];
	int i;

	for (i = 0; i < st->a; i++) {
		kaka_put_u32(v, (uint32_t)i);
		if (st_produce(st->part, (uint32_t)i + 1, v, 4, &rep) != 0) {
			st->result = 1; goto done;
		}
		if (!rep.ok) {
			if (rep.err != KAKA_ERR_MEMORY || rep.n_stored != 0) {
				st->result = 4; goto done;
			}
			st->u++;
		}
	}
	if (st->b) {
		if (st_fetch(st->part, 1000, 0, 65536, &rep) != 0) {
			st->result = 2; goto done;
		}
		free(rep.records);
	}
done:
	st_shutdown(st->part);
}

static int
metrics_run(int n_produce, int do_fetch, uint64_t *rejected)
{
	struct st st;
	int rc;
	memset(&st, 0, sizeof st);
	st.a = n_produce;
	st.b = do_fetch;
	broker_set_log_dir(NULL);
	rc = st_run(metrics_client, &st);
	if (rejected) *rejected = st.u;
	return rc;
}

int
broker_metrics_selftest(void)
{
	kaka_metrics_t m0, m1, m2;
	uint64_t rej = 0;

	kaka_metrics_init();
	kaka_metrics_mem_reset();
	kaka_metrics_set_mem_cap(0);

	kaka_metrics_get(&m0);
	if (metrics_run(5, 1, NULL) != 0) return 1;
	kaka_metrics_get(&m1);
	if (m1.produce_requests - m0.produce_requests != 5) return 2;
	if (m1.produce_records  - m0.produce_records  != 5) return 3;
	if (m1.produce_bytes    - m0.produce_bytes    != 20) return 4;
	if (m1.fetch_requests   - m0.fetch_requests   != 1) return 5;
	if (m1.fetch_records    - m0.fetch_records    != 5) return 6;
	if (m1.produce_p50_us < 0 || m1.fetch_p50_us < 0) return 7;

	/* In-memory mode, 12-byte cap: exactly 3 of 10 4-byte records fit
	 * and the other 7 are refused with KAKA_ERR_MEMORY. */
	kaka_metrics_mem_reset();
	kaka_metrics_set_mem_cap(12);
	if (metrics_run(10, 0, &rej) != 0) return 8;
	kaka_metrics_get(&m2);
	if (rej != 7) return 9;
	if (m2.produce_rejected - m1.produce_rejected != 7) return 10;
	if (m2.mem_used != 0) return 11;   /* released at partition exit */
	kaka_metrics_set_mem_cap(0);
	return 0;
}

/* ---- durability (E-M5) ---- */

static _Atomic int g_syncs;
static _Atomic int g_sync_fail;
static _Atomic int64_t g_sync_sleep;

static int
st_sync_hook(int fd)
{
	int64_t ns = atomic_load(&g_sync_sleep);
	atomic_fetch_add(&g_syncs, 1);
	if (ns > 0)
		(void)xtc_proc_sleep(ns);
	if (atomic_load(&g_sync_fail))
		return -5;               /* -EIO */
	return xtc_aio_fdatasync(fd);
}

static void
durable_client(void *arg)
{
	struct st *st = arg;
	struct part_reply rep;
	uint8_t v[4];
	int i, s0;

	/* (a) the ack of a produce comes after a sync. */
	s0 = atomic_load(&g_syncs);
	kaka_put_u32(v, 0);
	if (st_produce(st->part, 1, v, 4, &rep) != 0 || !rep.ok) {
		st->result = 1; goto done;
	}
	if (atomic_load(&g_syncs) == s0) { st->result = 2; goto done; }

	/* (b) group commit: 64 produces queued together share syncs. */
	s0 = atomic_load(&g_syncs);
	for (i = 1; i <= 64; i++) {
		uint8_t rec[16];
		kaka_put_u32(v, (uint32_t)i);
		if (st_send_produce(st->part, (uint32_t)i + 1, rec,
		    st_rec(rec, NULL, 0, v, 4), 1) != 0) {
			st->result = 3; goto done;
		}
	}
	for (i = 1; i <= 64; i++)
		if (await_reply((uint32_t)i + 1, &rep, sizeof rep) != 0 ||
		    !rep.ok) { st->result = 4; goto done; }
	st->a = atomic_load(&g_syncs) - s0;
	if (st->a < 1 || st->a >= 64) { st->result = 5; goto done; }

	/* (c) a failed sync fails the produce and every later one. */
	atomic_store(&g_sync_fail, 1);
	kaka_put_u32(v, 99);
	if (st_produce(st->part, 100, v, 4, &rep) != 0 || rep.ok ||
	    rep.err != KAKA_ERR_STORAGE) { st->result = 6; goto done; }
	atomic_store(&g_sync_fail, 0);
	if (st_produce(st->part, 101, v, 4, &rep) != 0 || rep.ok ||
	    rep.err != KAKA_ERR_STORAGE) { st->result = 7; goto done; }
	/* Records synced before the failure are still served; the
	 * unsynced one is not exposed. */
	if (st_fetch(st->part, 102, 0, 65536, &rep) != 0 || !rep.ok ||
	    st_count(&rep, 0, 1) != 65) { st->result = 8; goto done; }
	free(rep.records);
done:
	st_shutdown(st->part);
}

int
broker_durable_selftest(const char *dir)
{
	struct st st;
	int rc;
	memset(&st, 0, sizeof st);
	broker_set_log_dir(dir);
	broker_set_fsync_interval(0);
	plog_set_sync_hook(st_sync_hook);
	rc = st_run(durable_client, &st);
	plog_set_sync_hook(NULL);
	broker_set_log_dir(NULL);
	if (rc == 0)
		printf("       (64 queued produces took %d fdatasync(s))\n", st.a);
	return rc;
}

/* ---- live memory budget (E-m3) ---- */

static void
budget_client(void *arg)
{
	struct st *st = arg;
	struct part_reply rep;
	uint8_t v[100];
	uint64_t off = 0;
	int i;

	memset(v, 'x', sizeof v);
	for (i = 0; i < st->a; i++) {
		kaka_put_u32(v, (uint32_t)i);
		if (st_produce(st->part, (uint32_t)i + 1, v, sizeof v, &rep)
		    != 0) { st->result = 1; goto done; }
		if (!rep.ok) { st->result = 2; st->u = (uint64_t)i; goto done; }
	}
	/* Everything is still readable (mostly from disk). */
	while (off < (uint64_t)st->a) {
		long n;
		if (st_fetch(st->part, 100000 + (uint32_t)off, off, 65536,
		    &rep) != 0 || !rep.ok) { st->result = 3; goto done; }
		n = st_count(&rep, off, 1);
		free(rep.records);
		if (n <= 0) { st->result = 4; goto done; }
		off += (uint64_t)n;
	}
done:
	st_shutdown(st->part);
}

int
broker_budget_selftest(const char *dir)
{
	struct st st;
	kaka_metrics_t m;
	int rc;

	memset(&st, 0, sizeof st);
	st.a = 2000;                      /* 2000 x 100 B = 200 KB */
	kaka_metrics_init();
	kaka_metrics_mem_reset();
	kaka_metrics_set_mem_cap(16384);
	broker_set_log_dir(dir);
	broker_set_segment_bytes(4096);
	rc = st_run(budget_client, &st);
	kaka_metrics_get(&m);
	broker_set_segment_bytes(0);
	broker_set_log_dir(NULL);
	kaka_metrics_set_mem_cap(0);
	if (rc != 0) {
		fprintf(stderr, "budget: step %d at record %llu, mem_used %lld\n",
		    rc, (unsigned long long)st.u, (long long)m.mem_used);
		return rc;
	}
	return m.mem_used == 0 ? 0 : 20;
}

/* ---- abandoned request owns its payload (candidate UAF) ---- */

static void
timeout_client(void *arg)
{
	struct st *st = arg;
	struct part_reply rep;
	uint8_t *body[2];
	size_t blen = 0;
	int sv[2], i;
	xtc_pid_t part;
	long n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		st->result = 1; return;
	}
	/* Two PRODUCE bodies for topic "u": the first stalls in a 300 ms
	 * sync, the second queues behind it; both connections give up
	 * after 50 ms and free their buffers before the partition reads
	 * the second one. */
	for (i = 0; i < 2; i++) {
		uint8_t *p;
		uint8_t v[200];
		memset(v, 'A' + i, sizeof v);
		kaka_put_u32(v, (uint32_t)i);
		if ((body[i] = malloc(512)) == NULL) { st->result = 2; return; }
		p = body[i];
		kaka_put_u16(p, 1); p[2] = 'u'; p += 3;
		kaka_put_u32(p, 0); p += 4;
		kaka_put_u32(p, 1); p += 4;
		p += st_rec(p, "k", 1, v, sizeof v);
		blen = (size_t)(p - body[i]);
	}
	atomic_store(&g_sync_sleep, 300LL * 1000 * 1000);
	broker_set_request_timeout(50LL * 1000 * 1000);
	for (i = 0; i < 2; i++) {
		(void)conn_partition_req(sv[0], KAKA_PRODUCE, body[i], blen,
		    (uint32_t)i + 1);
		memset(body[i], 0xee, 512);
		free(body[i]);
	}
	atomic_store(&g_sync_sleep, 0);
	broker_set_request_timeout(30LL * 1000 * 1000 * 1000);
	if (partition_for("u", 1, 0, &part) != 0) { st->result = 3; goto out; }
	(void)xtc_proc_sleep(700LL * 1000 * 1000);
	if (st_fetch(part, 50, 0, 65536, &rep) != 0 || !rep.ok) {
		st->result = 4; goto out;
	}
	n = st_count(&rep, 0, 1);
	if (n != 2) st->result = 5;
	else {
		/* The second record's bytes are intact: 'B' * 196 after
		 * the 4-byte sequence. */
		const uint8_t *r = rep.records + 17;
		r += 16 + kaka_get_u32(r + 8) + kaka_get_u32(r + 12);
		if (kaka_get_u32(r + 12) != 200 || r[16 + 1 + 4] != 'B' ||
		    r[16 + 1 + 199] != 'B')
			st->result = 6;
	}
	free(rep.records);
out:
	(void)close(sv[0]);
	(void)close(sv[1]);
	st->a = 1;
}

static void
timeout_main(void *arg)
{
	struct st *st = arg;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	opts.name = "timeout-client";
	if (xtc_proc_spawn(g_loop, timeout_client, st, &opts, &pid) != XTC_OK) {
		st->result = 9; return;
	}
	while (!st->a)
		(void)xtc_proc_sleep(10LL * 1000 * 1000);
	broker_shutdown(0);
}

int
broker_timeout_selftest(const char *dir)
{
	struct st st;
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i;

	memset(&st, 0, sizeof st);
	if (xtc_loop_init(&loop) != XTC_OK) return -1;
	broker_set_loop(loop);
	broker_set_log_dir(dir);
	plog_set_sync_hook(st_sync_hook);
	opts.name = "timeout-main";
	if (xtc_proc_spawn(loop, timeout_main, &st, &opts, &pid) != XTC_OK)
		return -1;
	if (xtc_loop_run(loop) != XTC_OK) return -1;
	(void)xtc_loop_fini(loop);
	plog_set_sync_hook(NULL);
	broker_set_log_dir(NULL);
	atomic_store(&g_stopping, 0);
	for (i = 0; i < MAX_PARTITIONS; i++)
		g_parts[i].in_use = 0;
	return st.result;
}
