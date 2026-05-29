/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/broker.c
 *
 *	The broker's two process kinds and the partition registry,
 *	Phase 1.  This is where the libxtc actor model does the work:
 *
 *	  partition_proc -- one per (topic, partition).  Owns a plog.
 *	    Receives request messages (PRODUCE / FETCH) carrying a
 *	    reply pid + tag, mutates or reads the log, and replies.
 *	    Being the single owner, its appends need no lock: the
 *	    mailbox is the serialization point and offsets come out
 *	    monotonic.
 *
 *	  conn_proc -- one per client TCP connection.  Parks on the
 *	    socket with xtc_proc_wait_fd (idle connections cost no CPU),
 *	    reads frames, decodes them, issues a synchronous request to
 *	    the owning partition proc, and writes the response frame
 *	    back.
 *
 *	The conn->partition request is a small fixed control struct
 *	carrying borrowed pointers into the conn's read buffer; because
 *	the call is synchronous (conn awaits the reply before touching
 *	its buffer again) and plog_append copies the bytes, the borrow
 *	is safe.  This is the gen_server call pattern expressed with
 *	raw mailboxes.
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
#include "xtc_int.h"
#include "xtc_io.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#include "broker.h"
#include "frame.h"
#include "partition.h"

/* ---- conn <-> partition control messages ---- */

enum { REQ_PRODUCE = 1, REQ_FETCH = 2, REQ_SHUTDOWN = 3 };

struct part_req {
	uint8_t    op;          /* REQ_PRODUCE / REQ_FETCH */
	xtc_pid_t  reply;       /* conn proc to reply to */
	uint32_t   tag;         /* correlation tag */
	/* PRODUCE: borrowed decoded batch (valid for the sync call). */
	kaka_produce_t produce;
	/* FETCH */
	uint64_t   offset;
	uint32_t   max_bytes;
};

struct part_reply {
	uint32_t   tag;
	int        ok;          /* 1 = success, 0 = error */
	uint64_t   base_offset; /* PRODUCE: assigned base */
	/* FETCH replies stream records as a heap buffer the conn frees. */
	uint8_t   *records;     /* malloc'd RECORDS-frame body, or NULL */
	size_t     records_len;
	uint64_t   hwm;
};

/* ---- partition registry (Phase 1: small fixed table) ---- */

#define MAX_PARTITIONS 256

struct part_entry {
	char       topic[64];
	uint32_t   partition;
	xtc_pid_t  pid;
	int        in_use;
};

struct part_arg {            /* passed to a freshly spawned partition */
	char     topic[64];
	uint32_t partition;
};

static struct part_entry  g_parts[MAX_PARTITIONS];
static pthread_mutex_t     g_parts_lock = PTHREAD_MUTEX_INITIALIZER;
static xtc_loop_t         *g_loop;
static char                g_log_dir[256];   /* empty = in-memory */

/* ---- partition proc ---- */

static void
partition_proc(void *arg)
{
	struct part_arg *pa = arg;
	plog_t *log = NULL;
	void *msg;
	size_t msg_len;

	if (g_log_dir[0] != '\0') {
		/* Durable: <log_dir>/<topic>-<partition>/ with segmented
		 * files; recovery replays on (re)start. */
		char pdir[512];
		snprintf(pdir, sizeof pdir, "%s/%s-%u",
		    g_log_dir, pa->topic, pa->partition);
		(void)mkdir(g_log_dir, 0755);
		(void)mkdir(pdir, 0755);
		if (plog_create_ex(pdir, 0, &log) != 0) {
			XTC_LOG_ERROR_F("partition %s/%u: log open failed",
			    pa->topic, pa->partition);
			free(pa);
			return;
		}
	} else if (plog_create(&log) != 0) {
		XTC_LOG_ERROR_F("partition %s/%u: log create failed",
		    pa->topic, pa->partition);
		free(pa);
		return;
	}
	XTC_LOG_INFO_F("partition %s/%u up", pa->topic, pa->partition);

	for (;;) {
		struct part_req req;
		struct part_reply rep;
		if (xtc_recv(&msg, &msg_len, -1) != XTC_OK)
			continue;
		if (msg == NULL || msg_len < sizeof req) {
			if (msg) __os_free(msg);
			continue;
		}
		memcpy(&req, msg, sizeof req);
		__os_free(msg);

		memset(&rep, 0, sizeof rep);
		rep.tag = req.tag;

		if (req.op == REQ_SHUTDOWN) {
			rep.ok = 1;
			(void)xtc_send(req.reply, &rep, sizeof rep);
			break;          /* graceful partition shutdown */
		} else if (req.op == REQ_PRODUCE) {
			kaka_record_t r;
			int64_t base = -1, off;
			int got;
			while ((got = kaka_produce_next_record(&req.produce,
			    &r)) == 1) {
				off = plog_append(log, &r);
				if (off < 0) break;
				if (base < 0) base = off;
			}
			rep.ok = (base >= 0 || req.produce.n_records == 0);
			rep.base_offset = (base >= 0) ? (uint64_t)base : 0;
			rep.hwm = plog_high_water(log);
		} else if (req.op == REQ_FETCH) {
			/* Build a RECORDS body up to max_bytes. */
			size_t cap = req.max_bytes ? req.max_bytes : 65536;
			uint8_t *buf = malloc(cap);
			uint64_t off = req.offset;
			uint32_t n = 0;
			size_t used = 17;     /* header reserved (5 + 12) */
			if (buf != NULL) {
				kaka_record_t r;
				while (off < plog_high_water(log)) {
					long w;
					if (plog_read(log, off, &r) != 1) break;
					w = kaka_encode_record(buf + used,
					    cap - used, off, &r);
					if (w < 0) break;     /* buffer full */
					used += (size_t)w;
					n++; off++;
				}
				/* patch the streaming header now n is known */
				kaka_encode_records_header(buf, cap,
				    req.offset, n);
				kaka_frame_header(buf, cap, KAKA_RECORDS,
				    used - 5);
				rep.ok = 1;
				rep.records = buf;
				rep.records_len = used;
			}
			rep.hwm = plog_high_water(log);
		}

		(void)xtc_send(req.reply, &rep, sizeof rep);
	}
	plog_destroy(log);
	free(pa);
}

/* Resolve (or lazily spawn) the partition proc for topic/partition. */
static int
partition_for(const char *topic, uint16_t topic_len, uint32_t partition,
              xtc_pid_t *out)
{
	int i, free_slot = -1;
	char tkey[64];
	size_t tl = topic_len < sizeof tkey - 1 ? topic_len : sizeof tkey - 1;
	memcpy(tkey, topic, tl);
	tkey[tl] = '\0';

	(void)pthread_mutex_lock(&g_parts_lock);
	for (i = 0; i < MAX_PARTITIONS; i++) {
		if (g_parts[i].in_use) {
			if (g_parts[i].partition == partition &&
			    strcmp(g_parts[i].topic, tkey) == 0) {
				*out = g_parts[i].pid;
				(void)pthread_mutex_unlock(&g_parts_lock);
				return 0;
			}
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot < 0) {
		(void)pthread_mutex_unlock(&g_parts_lock);
		return -1;             /* table full */
	}
	/* Spawn a new partition proc. */
	{
		struct part_arg *pa = calloc(1, sizeof *pa);
		xtc_proc_opts_t opts = { 0 };
		xtc_pid_t pid;
		if (pa == NULL) {
			(void)pthread_mutex_unlock(&g_parts_lock);
			return -1;
		}
		memcpy(pa->topic, tkey, tl + 1);
		pa->partition = partition;
		opts.name = "kaka-partition";
		if (xtc_proc_spawn(g_loop, partition_proc, pa, &opts, &pid)
		    != XTC_OK) {
			free(pa);
			(void)pthread_mutex_unlock(&g_parts_lock);
			return -1;
		}
		g_parts[free_slot].in_use = 1;
		g_parts[free_slot].partition = partition;
		memcpy(g_parts[free_slot].topic, tkey, tl + 1);
		g_parts[free_slot].pid = pid;
		*out = pid;
	}
	(void)pthread_mutex_unlock(&g_parts_lock);
	return 0;
}

/* ---- connection proc ---- */

struct conn_arg { int fd; };

/* Read exactly one complete frame into *buf (grown as needed).
 * Returns frame length consumed, 0 on clean EOF, -1 on error. */
static long
conn_read_frame(int fd, uint8_t **buf, size_t *cap, size_t *have)
{
	for (;;) {
		uint8_t type;
		const uint8_t *body;
		size_t body_len;
		long n = kaka_frame_parse(*buf, *have, &type, &body, &body_len);
		if (n > 0) return n;
		if (n < 0) return -1;
		/* need more bytes */
		if (*have == *cap) {
			size_t ncap = *cap ? *cap * 2 : 4096;
			uint8_t *nb = realloc(*buf, ncap);
			if (nb == NULL) return -1;
			*buf = nb; *cap = ncap;
		}
		{
			ssize_t r;
			uint32_t revents = 0;
			r = recv(fd, (char *)(*buf + *have), *cap - *have, 0);
			if (r > 0) { *have += (size_t)r; continue; }
			if (r == 0) return 0;       /* peer closed */
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				(void)xtc_proc_wait_fd(fd, XTC_IO_READABLE,
				    -1, &revents);
				continue;
			}
			if (errno == EINTR) continue;
			return -1;
		}
	}
}

static int
conn_write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t w = send(fd, (const char *)(buf + off), len - off, 0);
		if (w > 0) { off += (size_t)w; continue; }
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(fd, XTC_IO_WRITABLE, -1, &revents);
			continue;
		}
		if (w < 0 && errno == EINTR) continue;
		return -1;
	}
	return 0;
}

static void
conn_proc(void *arg)
{
	struct conn_arg *ca = arg;
	int fd = ca->fd;
	uint8_t *buf = NULL;
	size_t cap = 0, have = 0;
	static _Atomic uint32_t g_tag;
	free(ca);

	for (;;) {
		uint8_t type;
		const uint8_t *body;
		size_t body_len;
		long fn = conn_read_frame(fd, &buf, &cap, &have);
		if (fn <= 0) break;
		(void)kaka_frame_parse(buf, have, &type, &body, &body_len);

		if (type == KAKA_PRODUCE || type == KAKA_FETCH) {
			struct part_req req;
			void *reply_msg;
			size_t reply_len;
			xtc_pid_t ppid;
			const char *topic;
			uint16_t topic_len;
			uint32_t partition;

			memset(&req, 0, sizeof req);
			req.reply = xtc_self();
			req.tag = atomic_fetch_add(&g_tag, 1) + 1;

			if (type == KAKA_PRODUCE) {
				if (kaka_decode_produce(body, body_len,
				    &req.produce) != 0)
					goto proto_err;
				req.op = REQ_PRODUCE;
				topic = req.produce.topic;
				topic_len = req.produce.topic_len;
				partition = req.produce.partition;
			} else {
				kaka_fetch_t f;
				if (kaka_decode_fetch(body, body_len, &f) != 0)
					goto proto_err;
				req.op = REQ_FETCH;
				req.offset = f.offset;
				req.max_bytes = f.max_bytes;
				topic = f.topic;
				topic_len = f.topic_len;
				partition = f.partition;
			}

			if (partition_for(topic, topic_len, partition, &ppid)
			    != 0)
				goto proto_err;
			if (xtc_send(ppid, &req, sizeof req) != XTC_OK)
				goto proto_err;

			/* Await the matching reply. */
			if (xtc_recv(&reply_msg, &reply_len, 5000LL * 1000000)
			    != XTC_OK || reply_msg == NULL)
				goto proto_err;
			{
				struct part_reply rep;
				memcpy(&rep, reply_msg, sizeof rep);
				__os_free(reply_msg);

				if (type == KAKA_PRODUCE) {
					uint8_t ack[13];
					kaka_encode_produce_ack(ack, sizeof ack,
					    rep.base_offset);
					if (conn_write_all(fd, ack, 13) != 0)
						break;
				} else {
					if (rep.records != NULL) {
						(void)conn_write_all(fd,
						    rep.records,
						    rep.records_len);
						free(rep.records);
					}
				}
			}
			/* consume the frame from the buffer */
			memmove(buf, buf + fn, have - (size_t)fn);
			have -= (size_t)fn;
			continue;
		}

proto_err:
		{
			uint8_t err[64];
			long e = kaka_encode_error(err, sizeof err, 1,
			    "bad request");
			if (e > 0) (void)conn_write_all(fd, err, (size_t)e);
			break;
		}
	}

	free(buf);
	(void)close(fd);
}

/* ---- public entry points ---- */

void
broker_set_loop(xtc_loop_t *loop)
{
	g_loop = loop;
}

void
broker_set_log_dir(const char *dir)
{
	if (dir != NULL && dir[0] != '\0')
		snprintf(g_log_dir, sizeof g_log_dir, "%s", dir);
	else
		g_log_dir[0] = '\0';
}

int
broker_spawn_conn(xtc_loop_t *loop, int fd)
{
	struct conn_arg *ca = calloc(1, sizeof *ca);
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	if (ca == NULL) return -1;
	ca->fd = fd;
	opts.name = "kaka-conn";
	if (xtc_proc_spawn(loop, conn_proc, ca, &opts, &pid) != XTC_OK) {
		free(ca);
		return -1;
	}
	return 0;
}

/* ---- in-process self-test ----
 *
 * Drives the full PRODUCE / FETCH request-reply path through a real
 * xtc loop with no socket and no separate daemon, so it can run
 * under the example's `make test` without the fd-inheritance hang
 * that a networked client+daemon test hits.  A client proc produces
 * a batch to a partition proc, fetches it back, checks the offsets
 * and bytes, then shuts the partition down so the loop terminates.
 */

struct selftest_state {
	int      result;        /* 0 = pass, nonzero = first failed step */
	xtc_pid_t part;
};

static void
selftest_client(void *arg)
{
	struct selftest_state *st = arg;
	struct part_req req;
	struct part_reply rep;
	void *msg; size_t mlen;

	/* Build a 3-record PRODUCE batch in a local buffer and decode it
	 * into req.produce (borrowed pointers stay valid for the call). */
	static uint8_t body[256];
	uint8_t *p = body;
	const char *topic = "t";
	kaka_put_u16(p, 1); p += 2; *p++ = (uint8_t)topic[0];
	kaka_put_u32(p, 0); p += 4;            /* partition 0 */
	kaka_put_u32(p, 3); p += 4;            /* 3 records */
	kaka_put_u32(p, 1); p += 4; kaka_put_u32(p, 1); p += 4;
	*p++ = 'a'; *p++ = 'A';
	kaka_put_u32(p, 0); p += 4; kaka_put_u32(p, 2); p += 4;
	*p++ = 'b'; *p++ = 'B';
	kaka_put_u32(p, 1); p += 4; kaka_put_u32(p, 1); p += 4;
	*p++ = 'c'; *p++ = 'C';

	memset(&req, 0, sizeof req);
	req.op = REQ_PRODUCE;
	req.reply = xtc_self();
	req.tag = 1;
	if (kaka_decode_produce(body, (size_t)(p - body), &req.produce) != 0) {
		st->result = 1; goto done;
	}
	if (xtc_send(st->part, &req, sizeof req) != XTC_OK) { st->result = 2; goto done; }
	if (xtc_recv(&msg, &mlen, 2000LL*1000000) != XTC_OK || msg == NULL) {
		st->result = 3; goto done;
	}
	memcpy(&rep, msg, sizeof rep); __os_free(msg);
	if (!rep.ok || rep.base_offset != 0 || rep.hwm != 3) { st->result = 4; goto done; }

	/* FETCH from offset 0. */
	memset(&req, 0, sizeof req);
	req.op = REQ_FETCH; req.reply = xtc_self(); req.tag = 2;
	req.offset = 0; req.max_bytes = 65536;
	if (xtc_send(st->part, &req, sizeof req) != XTC_OK) { st->result = 5; goto done; }
	if (xtc_recv(&msg, &mlen, 2000LL*1000000) != XTC_OK || msg == NULL) {
		st->result = 6; goto done;
	}
	memcpy(&rep, msg, sizeof rep); __os_free(msg);
	if (!rep.ok || rep.records == NULL) { st->result = 7; goto done; }
	{
		uint8_t type; const uint8_t *fb; size_t fl;
		long fn = kaka_frame_parse(rep.records, rep.records_len,
		    &type, &fb, &fl);
		uint32_t n;
		if (fn <= 0 || type != KAKA_RECORDS) { st->result = 8; }
		else {
			n = kaka_get_u32(fb + 8);   /* n_records after base */
			if (n != 3) st->result = 9;
		}
		free(rep.records);
	}

done:
	/* Shut the partition down so xtc_loop_run can return. */
	memset(&req, 0, sizeof req);
	req.op = REQ_SHUTDOWN; req.reply = xtc_self(); req.tag = 99;
	if (xtc_send(st->part, &req, sizeof req) == XTC_OK)
		(void)xtc_recv(&msg, &mlen, 1000LL*1000000), (msg ? __os_free(msg) : (void)0);
}

int
broker_selftest(void)
{
	xtc_loop_t *loop = NULL;
	struct selftest_state st;
	struct part_arg *pa;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t part, client;

	memset(&st, 0, sizeof st);
	if (xtc_loop_init(&loop) != XTC_OK) return -1;

	pa = calloc(1, sizeof *pa);
	if (pa == NULL) return -1;
	pa->topic[0] = 't'; pa->topic[1] = '\0'; pa->partition = 0;
	opts.name = "selftest-partition";
	if (xtc_proc_spawn(loop, partition_proc, pa, &opts, &part) != XTC_OK) {
		free(pa); return -1;
	}
	st.part = part;
	opts.name = "selftest-client";
	if (xtc_proc_spawn(loop, selftest_client, &st, &opts, &client)
	    != XTC_OK)
		return -1;

	if (xtc_loop_run(loop) != XTC_OK) return -1;
	return st.result;
}
