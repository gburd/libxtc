/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/tail.c
 *	The runtime microscope (xtc_tail): a bounded ring of individual
 *	runtime events, gated by a source mask.  See src/inc/xtc_tail.h.
 *
 *	Hot path: __xtc_tail_emit is one relaxed-atomic mask load + early
 *	return when the source is disabled, so a build with tail off pays a
 *	single predictable branch.  When on, a record is appended under a
 *	small lock (correctness over the last few ns; the microscope is a
 *	debug tool, not a steady-state tax).
 */

#include "xtc_int.h"
#include "xtc_tail.h"
#include "tail_int.h"        /* __xtc_tail_emit / __xtc_tail_on (internal) */
#include "preempt_int.h"   /* __xtc_mtx_lock/unlock */
#include "os_time.h"
#include "os_sharp.h"    /* __os_env_get */
#include "xtc_fs.h"       /* xtc_fs_open/close (portable file open) */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define XTC_TAIL_RING 16384u

static _Atomic unsigned __tail_mask;    /* enabled sources; 0 = off */
static pthread_mutex_t  __tail_lock = PTHREAD_MUTEX_INITIALIZER;
static xtc_tail_rec_t   __tail_ring[XTC_TAIL_RING];
static uint64_t         __tail_seq;     /* total records ever written */

static uint64_t
__tail_now_ns(void)
{
	int64_t ns = 0;
	(void)__os_clock_mono(&ns);
	return (uint64_t)ns;
}

/*
 * Internal emit, called from the runtime hook points.  `source` is a
 * single XTC_TAIL_* bit; the event is dropped unless that source is
 * enabled.  Never fails, never blocks a fiber.
 */
void
__xtc_tail_emit(unsigned source, unsigned kind, xtc_pid_t pid, uint64_t detail)
{
	unsigned mask = atomic_load_explicit(&__tail_mask, memory_order_relaxed);
	xtc_tail_rec_t *r;

	if ((mask & source) == 0)
		return;                 /* source off: one branch, done */

	(void)__xtc_mtx_lock(&__tail_lock);
	r = &__tail_ring[__tail_seq % XTC_TAIL_RING];
	r->ts_ns = __tail_now_ns();
	r->source = source;
	r->kind = kind;
	r->pid = pid;
	r->detail = detail;
	__tail_seq++;
	(void)__xtc_mtx_unlock(&__tail_lock);
}

unsigned
xtc_tail_enable(unsigned source_mask)
{
	return atomic_exchange_explicit(&__tail_mask, source_mask & XTC_TAIL_ALL,
	    memory_order_release);
}

/* Fast predicate for hook points that must do extra work (e.g. read a
 * clock) ONLY when a source is enabled -- so a disabled tail is a single
 * relaxed load + branch and perturbs nothing (crucially, it keeps the
 * clock-reads out of the deterministic-sim recv path unless tail is on). */
int
__xtc_tail_on(unsigned source)
{
	return (atomic_load_explicit(&__tail_mask, memory_order_relaxed)
	    & source) != 0;
}

void
xtc_tail_disable(void)
{
	atomic_store_explicit(&__tail_mask, 0u, memory_order_release);
}

int
xtc_tail_reset(void)
{
	(void)__xtc_mtx_lock(&__tail_lock);
	__tail_seq = 0;
	(void)__xtc_mtx_unlock(&__tail_lock);
	return XTC_OK;
}

size_t
xtc_tail_count(void)
{
	uint64_t n;
	(void)__xtc_mtx_lock(&__tail_lock);
	n = __tail_seq < XTC_TAIL_RING ? __tail_seq : XTC_TAIL_RING;
	(void)__xtc_mtx_unlock(&__tail_lock);
	return (size_t)n;
}

/* Snapshot the ring (oldest first) into a caller-provided visitor.  The
 * snapshot is taken under the lock, then visited outside it. */
static int
__tail_snapshot(xtc_tail_rec_t **out, size_t *out_n)
{
	xtc_tail_rec_t *snap;
	uint64_t n, start, i;
	int rc;

	(void)__xtc_mtx_lock(&__tail_lock);
	n = __tail_seq < XTC_TAIL_RING ? __tail_seq : XTC_TAIL_RING;
	start = __tail_seq < XTC_TAIL_RING ? 0 : __tail_seq % XTC_TAIL_RING;
	if (n == 0) {
		(void)__xtc_mtx_unlock(&__tail_lock);
		*out = NULL; *out_n = 0;
		return XTC_OK;
	}
	if ((rc = __os_malloc((size_t)n * sizeof(*snap), (void **)&snap))
	    != XTC_OK) {
		(void)__xtc_mtx_unlock(&__tail_lock);
		return rc;
	}
	for (i = 0; i < n; i++)
		snap[i] = __tail_ring[(start + i) % XTC_TAIL_RING];
	(void)__xtc_mtx_unlock(&__tail_lock);
	*out = snap;
	*out_n = (size_t)n;
	return XTC_OK;
}

int
xtc_tail_read(xtc_tail_fn cb, void *user)
{
	xtc_tail_rec_t *snap = NULL;
	size_t n = 0, i;
	int rc;

	if (cb == NULL) return XTC_E_INVAL;
	if ((rc = __tail_snapshot(&snap, &n)) != XTC_OK) return rc;
	for (i = 0; i < n; i++)
		if (cb(&snap[i], user) != 0)
			break;
	if (snap != NULL) __os_free(snap);
	return XTC_OK;
}

/* Write all `len` bytes to fd, retrying short writes. */
static int
__tail_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, p + off, len - off); /* XTC_BLOCKING_OK: explicit user-invoked diagnostic dump to a caller-chosen fd */
		if (w < 0) return XTC_E_IO;
		if (w == 0) return XTC_E_IO;
		off += (size_t)w;
	}
	return XTC_OK;
}

/* Append an unsigned value as LEB128 varint to buf[*off].  buf must have
 * room for 10 bytes.  1 byte for values < 128, growing 7 bits at a time. */
static void
__leb128(uint8_t *buf, size_t *off, uint64_t v)
{
	do {
		uint8_t b = (uint8_t)(v & 0x7Fu);
		v >>= 7;
		if (v != 0) b |= 0x80u;
		buf[(*off)++] = b;
	} while (v != 0);
}

/* Store a u32/u64 little-endian into buf. */
static void
__tail_le32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static void
__tail_le64(uint8_t *b, uint64_t v)
{
	__tail_le32(b, (uint32_t)v);
	__tail_le32(b + 4, (uint32_t)(v >> 32));
}

/* PUBLIC: uint64_t xtc_tail_dropped __P((void)); */
uint64_t
xtc_tail_dropped(void)
{
	uint64_t seq;

	(void)__xtc_mtx_lock(&__tail_lock);
	seq = __tail_seq;
	(void)__xtc_mtx_unlock(&__tail_lock);
	return seq > (uint64_t)XTC_TAIL_RING ? seq - (uint64_t)XTC_TAIL_RING : 0;
}

int
xtc_tail_dump(int fd)
{
	xtc_tail_rec_t *snap = NULL;
	size_t n = 0, i;
	uint8_t hdr[24];   /* magic4 + version4 + flags4 + count4 + base_ts8 */
	uint64_t prev_ts = 0;
	int rc;

	if (fd < 0) return XTC_E_INVAL;
	if ((rc = __tail_snapshot(&snap, &n)) != XTC_OK) return rc;

	/* Portable little-endian header (no struct memcpy). */
	__tail_le32(hdr + 0, XTC_TAIL_MAGIC);
	__tail_le32(hdr + 4, XTC_TAIL_VERSION);
	__tail_le32(hdr + 8, XTC_TAIL_FLAG_LE);
	__tail_le32(hdr + 12, (uint32_t)n);
	__tail_le64(hdr + 16, n > 0 ? snap[0].ts_ns : 0);
	rc = __tail_write_all(fd, hdr, sizeof hdr);
	if (rc != XTC_OK) goto out;

	prev_ts = n > 0 ? snap[0].ts_ns : 0;
	for (i = 0; i < n; i++) {
		/* Worst case per event: 2 fixed bytes + 5 varints * 10 = 52. */
		uint8_t buf[64];
		size_t off = 0;
		uint64_t dts = snap[i].ts_ns >= prev_ts ?
		    snap[i].ts_ns - prev_ts : 0;
		prev_ts = snap[i].ts_ns;
		buf[off++] = (uint8_t)snap[i].kind;
		buf[off++] = (uint8_t)snap[i].source;
		__leb128(buf, &off, dts);
		__leb128(buf, &off, (uint64_t)snap[i].pid.loop_id);
		__leb128(buf, &off, (uint64_t)snap[i].pid.local_id);
		__leb128(buf, &off, (uint64_t)snap[i].pid.gen);
		__leb128(buf, &off, snap[i].detail);
		if ((rc = __tail_write_all(fd, buf, off)) != XTC_OK)
			goto out;
	}
out:
	if (snap != NULL) __os_free(snap);
	return rc;
}

/* ------------------------------------------------------------------ *
 * dial9 trace format (TRC\0 v1) emission.
 *
 * xtc_tail_dump writes the native XTCL format for tools/xtc-tail.py.
 * xtc_tail_dump_dial9 writes the dial9 wire format so a libxtc trace opens
 * in the dial9 GUI viewer unchanged -- see .agent/dial9-SPEC-v1.md for the
 * spec and .agent/XTC_TAIL_DIAL9_PLAN_2026-09-12.md for the plan.  The
 * viewer dispatches on the SCHEMA NAME string, so a libxtc event kind is
 * registered under a dial9 BUILT-IN name where a runtime analogue exists
 * (PollStartEvent, TaskSpawnEvent, TaskTerminateEvent, WakeEventEvent) --
 * which lights up the viewer's native worker/poll/wake timeline -- and
 * under a libxtc-specific Xtc* name otherwise (the io_uring reap/submit
 * chain), which the viewer shows as a custom event pinned to the right
 * worker and task.
 *
 * Field layouts match dial9's built-ins EXACTLY (Phase 2): worker_id and
 * task_id wire as Varint(u64), local_queue as u8, spawn_loc/runtime_name as
 * a PooledString (u32 pool id resolved by a String Pool frame).  libxtc's
 * proc identity {loop_id, local_id, gen} is packed into one u64 task_id so
 * the GUI groups a proc's events; worker_id is the loop's exec id.
 *
 * Streams over the SAME per-event snapshot the XTCL dump takes (no extra
 * allocator on the write path); all values little-endian, LEB128 varints.
 * ------------------------------------------------------------------ */

#define XTC_D9_MAGIC0 0x54  /* 'T' */
#define XTC_D9_MAGIC1 0x52  /* 'R' */
#define XTC_D9_MAGIC2 0x43  /* 'C' */
#define XTC_D9_MAGIC3 0x00  /* '\0' */
#define XTC_D9_VERSION 0x01

/* Frame tags. */
#define XTC_D9_FRAME_SCHEMA   0x01
#define XTC_D9_FRAME_EVENT    0x02
#define XTC_D9_FRAME_STRPOOL  0x03
#define XTC_D9_FRAME_TS_RESET 0x05

/* Field type tags (SPEC Field Types table). */
#define XTC_D9_FT_I64          1
#define XTC_D9_FT_BOOL         3
#define XTC_D9_FT_POOLED_STR   7
#define XTC_D9_FT_VARINT       9
#define XTC_D9_FT_U8          11
#define XTC_D9_FT_U32         13

#define XTC_D9_DELTA_MAX 16777215u   /* u24 max before a reset frame */

/*
 * Dense type_id assignment.  The wire type_id is arbitrary (the viewer keys
 * on the NAME), so we reuse the enum xtc_tail_kind value as the type_id for
 * SCHED kinds, and reserve high ids for the framing schemas we synthesize.
 */
#define XTC_D9_TID_CLOCK_SYNC   200
#define XTC_D9_TID_SEG_META     201
#define XTC_D9_MAX_KIND          14   /* one past XTC_TAIL_POLL_FULL */

/* One field of a schema: wire name + field-type tag. */
struct xtc_d9_field { const char *name; uint8_t type; };

/*
 * The schema for a given kind: viewer-facing name + its field list.  Field
 * order here is the exact wire order the event frame must follow.  The
 * dial9 built-ins (poll/spawn/terminate/wake) match dial9's own layouts so
 * the GUI renders them natively; the Xtc* customs carry loop_id + task_id +
 * a kind-specific detail so they still pin to a worker/task on the timeline.
 */
struct xtc_d9_schema_def {
	const char           *name;
	const struct xtc_d9_field *fields;
	unsigned              nfields;
};

/* dial9 built-in field lists. */
static const struct xtc_d9_field XTC_D9_F_POLL_START[] = {
	{ "worker_id",   XTC_D9_FT_VARINT },
	{ "local_queue", XTC_D9_FT_U8 },
	{ "task_id",     XTC_D9_FT_VARINT },
	{ "spawn_loc",   XTC_D9_FT_POOLED_STR },
};
static const struct xtc_d9_field XTC_D9_F_TASK_SPAWN[] = {
	{ "task_id",      XTC_D9_FT_VARINT },
	{ "spawn_loc",    XTC_D9_FT_POOLED_STR },
	{ "instrumented", XTC_D9_FT_BOOL },
};
static const struct xtc_d9_field XTC_D9_F_TASK_TERM[] = {
	{ "task_id", XTC_D9_FT_VARINT },
};
static const struct xtc_d9_field XTC_D9_F_WAKE[] = {
	{ "waker_task_id", XTC_D9_FT_VARINT },
	{ "woken_task_id", XTC_D9_FT_VARINT },
	{ "target_worker", XTC_D9_FT_U8 },
};
/* libxtc-specific custom events: worker + task + a kind detail. */
static const struct xtc_d9_field XTC_D9_F_XTC[] = {
	{ "worker_id", XTC_D9_FT_VARINT },
	{ "task_id",   XTC_D9_FT_VARINT },
	{ "detail",    XTC_D9_FT_I64 },
};
/* framing schemas. */
static const struct xtc_d9_field XTC_D9_F_CLOCK[] = {
	{ "realtime_ns", XTC_D9_FT_VARINT },
};
static const struct xtc_d9_field XTC_D9_F_SEGMETA[] = {
	{ "service",   XTC_D9_FT_POOLED_STR },
	{ "backend",   XTC_D9_FT_POOLED_STR },
};

#define XTC_D9_NF(a) ((unsigned)(sizeof (a) / sizeof (a)[0]))

/* Kind -> schema definition, or NULL to skip. */
static const struct xtc_d9_schema_def *
__xtc_d9_kind_schema(unsigned kind)
{
	static const struct xtc_d9_schema_def poll_start =
	    { "PollStartEvent", XTC_D9_F_POLL_START, XTC_D9_NF(XTC_D9_F_POLL_START) };
	static const struct xtc_d9_schema_def task_spawn =
	    { "TaskSpawnEvent", XTC_D9_F_TASK_SPAWN, XTC_D9_NF(XTC_D9_F_TASK_SPAWN) };
	static const struct xtc_d9_schema_def task_term =
	    { "TaskTerminateEvent", XTC_D9_F_TASK_TERM, XTC_D9_NF(XTC_D9_F_TASK_TERM) };
	static const struct xtc_d9_schema_def wake =
	    { "WakeEventEvent", XTC_D9_F_WAKE, XTC_D9_NF(XTC_D9_F_WAKE) };
	static const struct xtc_d9_schema_def xtc_park =
	    { "XtcParkEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_send =
	    { "XtcSendEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_recv =
	    { "XtcRecvEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_hwm =
	    { "XtcMailboxHwmEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_lp =
	    { "XtcLoopPollEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_pt =
	    { "XtcParkTaskEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_reap =
	    { "XtcReapEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_sub =
	    { "XtcSubmitEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_subf =
	    { "XtcSubmitFailEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	static const struct xtc_d9_schema_def xtc_pf =
	    { "XtcPollFullEvent", XTC_D9_F_XTC, XTC_D9_NF(XTC_D9_F_XTC) };
	switch (kind) {
	case XTC_TAIL_RUN:        return &poll_start;
	case XTC_TAIL_SPAWN:      return &task_spawn;
	case XTC_TAIL_EXIT:       return &task_term;
	case XTC_TAIL_WAKE:       return &wake;
	case XTC_TAIL_PARK:       return &xtc_park;
	case XTC_TAIL_SEND:       return &xtc_send;
	case XTC_TAIL_RECV:       return &xtc_recv;
	case XTC_TAIL_MBOX_HWM:   return &xtc_hwm;
	case XTC_TAIL_LOOP_POLL:  return &xtc_lp;
	case XTC_TAIL_PARK_TASK:  return &xtc_pt;
	case XTC_TAIL_REAP:       return &xtc_reap;
	case XTC_TAIL_SUBMIT:     return &xtc_sub;
	case XTC_TAIL_SUBMIT_FAIL:return &xtc_subf;
	case XTC_TAIL_POLL_FULL:  return &xtc_pf;
	default:                  return NULL;
	}
}

/* Pack a proc pid into a single u64 task_id so the GUI groups a proc's
 * events: gen<<48 | loop<<32 | local.  Stable and collision-free within a
 * run (gen distinguishes reused local_ids). */
static uint64_t
__xtc_d9_task_id(xtc_pid_t p)
{
	return ((uint64_t)p.gen << 48) |
	       ((uint64_t)p.loop_id << 32) |
	       (uint64_t)p.local_id;
}

/* Append a u16-length-prefixed name (schema/field names). */
static void
__xtc_d9_put_name(uint8_t *buf, size_t *off, const char *s)
{
	size_t l = strlen(s);
	buf[(*off)++] = (uint8_t)(l & 0xff);
	buf[(*off)++] = (uint8_t)(l >> 8);
	memcpy(buf + *off, s, l); *off += l;
}

/* Write a schema frame from a schema_def. */
static int
__xtc_d9_write_schema(int fd, uint16_t type_id,
    const struct xtc_d9_schema_def *def)
{
	uint8_t buf[512];
	size_t off = 0;
	unsigned f;

	buf[off++] = XTC_D9_FRAME_SCHEMA;
	buf[off++] = (uint8_t)(type_id & 0xff);
	buf[off++] = (uint8_t)(type_id >> 8);
	__xtc_d9_put_name(buf, &off, def->name);
	buf[off++] = 1;                              /* has_timestamp */
	buf[off++] = (uint8_t)(def->nfields & 0xff); /* field_count u16 */
	buf[off++] = (uint8_t)(def->nfields >> 8);
	for (f = 0; f < def->nfields; f++) {
		__xtc_d9_put_name(buf, &off, def->fields[f].name);
		buf[off++] = def->fields[f].type;
	}
	return __tail_write_all(fd, buf, off);
}

/* Write a one-entry String Pool frame (pool_id -> string). */
static int
__xtc_d9_write_strpool(int fd, uint32_t pool_id, const char *s)
{
	uint8_t buf[256];
	size_t off = 0, l = strlen(s);
	if (l > 200) l = 200;
	buf[off++] = XTC_D9_FRAME_STRPOOL;
	__tail_le32(buf + off, 1); off += 4;         /* count */
	__tail_le32(buf + off, pool_id); off += 4;
	__tail_le32(buf + off, (uint32_t)l); off += 4;
	memcpy(buf + off, s, l); off += l;
	return __tail_write_all(fd, buf, off);
}

/* Emit the event-frame header (tag, type_id, u24 delta) into buf. */
static size_t
__xtc_d9_evhdr(uint8_t *buf, uint16_t type_id, uint64_t delta)
{
	size_t off = 0;
	buf[off++] = XTC_D9_FRAME_EVENT;
	buf[off++] = (uint8_t)(type_id & 0xff);
	buf[off++] = (uint8_t)(type_id >> 8);
	buf[off++] = (uint8_t)(delta & 0xff);
	buf[off++] = (uint8_t)((delta >> 8) & 0xff);
	buf[off++] = (uint8_t)((delta >> 16) & 0xff);
	return off;
}

/* PUBLIC: int xtc_tail_dump_dial9 __P((int)); */
int
xtc_tail_dump_dial9(int fd)
{
	xtc_tail_rec_t *snap = NULL;
	size_t n = 0, i;
	uint8_t hdr[5];
	uint8_t seen[XTC_D9_MAX_KIND];
	uint64_t base_ts = 0;
	int have_base = 0;
	int rc;
	/* Pool id 1 = the spawn_loc/service string (a single interned name,
	 * since libxtc does not carry a per-proc source location).  Pool id 2
	 * = the backend name.  Emitted up front. */
	const uint32_t POOL_LOC = 1, POOL_BACKEND = 2;

	if (fd < 0) return XTC_E_INVAL;
	if ((rc = __tail_snapshot(&snap, &n)) != XTC_OK) return rc;
	memset(seen, 0, sizeof seen);

	/* Header. */
	hdr[0] = XTC_D9_MAGIC0; hdr[1] = XTC_D9_MAGIC1;
	hdr[2] = XTC_D9_MAGIC2; hdr[3] = XTC_D9_MAGIC3;
	hdr[4] = XTC_D9_VERSION;
	if ((rc = __tail_write_all(fd, hdr, sizeof hdr)) != XTC_OK) goto out;

	/* String pool: the interned strings the schemas reference. */
	if ((rc = __xtc_d9_write_strpool(fd, POOL_LOC, "xtc-proc")) != XTC_OK)
		goto out;
	{
		const char *bk = xtc_io_backend_name();
		if ((rc = __xtc_d9_write_strpool(fd, POOL_BACKEND,
		    bk != NULL ? bk : "unknown")) != XTC_OK) goto out;
	}

	/* ClockSync + SegmentMetadata framing at the trace base time, so the
	 * viewer can recover wall clock and show the service/backend.  Uses
	 * the FIRST event's monotonic stamp as the base and pairs it with a
	 * wall-clock read (xtc_clock_real is NOT determinism-guarded, but this
	 * dump is an explicit user call off any sim-reachable path). */
	{
		static const struct xtc_d9_schema_def clock_def =
		    { "ClockSyncEvent", XTC_D9_F_CLOCK, XTC_D9_NF(XTC_D9_F_CLOCK) };
		static const struct xtc_d9_schema_def seg_def =
		    { "SegmentMetadataEvent", XTC_D9_F_SEGMETA, XTC_D9_NF(XTC_D9_F_SEGMETA) };
		uint64_t t0 = (n > 0) ? snap[0].ts_ns : 0;
		uint64_t rt = (uint64_t)xtc_clock_real();
		uint8_t buf[64];
		size_t off;
		uint8_t rst[9];

		if ((rc = __xtc_d9_write_schema(fd, XTC_D9_TID_CLOCK_SYNC,
		    &clock_def)) != XTC_OK) goto out;
		if ((rc = __xtc_d9_write_schema(fd, XTC_D9_TID_SEG_META,
		    &seg_def)) != XTC_OK) goto out;
		/* base the timestamp stream at t0 */
		rst[0] = XTC_D9_FRAME_TS_RESET;
		__tail_le64(rst + 1, t0);
		if ((rc = __tail_write_all(fd, rst, sizeof rst)) != XTC_OK) goto out;
		base_ts = t0; have_base = 1;
		/* ClockSyncEvent { realtime_ns } */
		off = __xtc_d9_evhdr(buf, XTC_D9_TID_CLOCK_SYNC, 0);
		__leb128(buf, &off, rt);
		if ((rc = __tail_write_all(fd, buf, off)) != XTC_OK) goto out;
		/* SegmentMetadataEvent { service=POOL_LOC, backend=POOL_BACKEND } */
		off = __xtc_d9_evhdr(buf, XTC_D9_TID_SEG_META, 0);
		__tail_le32(buf + off, POOL_LOC); off += 4;
		__tail_le32(buf + off, POOL_BACKEND); off += 4;
		if ((rc = __tail_write_all(fd, buf, off)) != XTC_OK) goto out;
	}

	for (i = 0; i < n; i++) {
		unsigned kind = snap[i].kind;
		const struct xtc_d9_schema_def *def;
		uint8_t buf[96];
		size_t off;
		uint64_t ts = snap[i].ts_ns;
		uint64_t delta, task_id, worker_id;

		if (kind >= XTC_D9_MAX_KIND) continue;
		def = __xtc_d9_kind_schema(kind);
		if (def == NULL) continue;

		if (!seen[kind]) {
			if ((rc = __xtc_d9_write_schema(fd, (uint16_t)kind,
			    def)) != XTC_OK) goto out;
			seen[kind] = 1;
		}

		/* Timestamp base + reset (spec's three triggers). */
		if (!have_base || ts < base_ts ||
		    ts - base_ts > XTC_D9_DELTA_MAX) {
			uint8_t rst[9];
			rst[0] = XTC_D9_FRAME_TS_RESET;
			__tail_le64(rst + 1, ts);
			if ((rc = __tail_write_all(fd, rst, sizeof rst))
			    != XTC_OK) goto out;
			base_ts = ts; have_base = 1;
		}
		delta = ts - base_ts;
		base_ts = ts;

		task_id = __xtc_d9_task_id(snap[i].pid);
		worker_id = (uint64_t)snap[i].pid.loop_id;
		off = __xtc_d9_evhdr(buf, (uint16_t)kind, delta);

		/* Fields in the exact schema order for this kind. */
		switch (kind) {
		case XTC_TAIL_RUN:   /* PollStartEvent */
			__leb128(buf, &off, worker_id);          /* worker_id */
			buf[off++] = 0;                          /* local_queue */
			__leb128(buf, &off, task_id);            /* task_id */
			__tail_le32(buf + off, POOL_LOC); off += 4; /* spawn_loc */
			break;
		case XTC_TAIL_SPAWN: /* TaskSpawnEvent */
			__leb128(buf, &off, task_id);            /* task_id */
			__tail_le32(buf + off, POOL_LOC); off += 4; /* spawn_loc */
			buf[off++] = 0;                          /* instrumented */
			break;
		case XTC_TAIL_EXIT:  /* TaskTerminateEvent */
			__leb128(buf, &off, task_id);            /* task_id */
			break;
		case XTC_TAIL_WAKE:  /* WakeEventEvent */
			/* dispatch has no waker task; woken task ptr is in
			 * detail.  Use 0 for waker, the detail as woken. */
			__leb128(buf, &off, 0);                  /* waker_task_id */
			__leb128(buf, &off, snap[i].detail);     /* woken_task_id */
			buf[off++] = (uint8_t)(worker_id > 254 ? 255 :
			    worker_id);                          /* target_worker */
			break;
		default:             /* Xtc* custom: worker, task, detail */
			__leb128(buf, &off, worker_id);
			__leb128(buf, &off, task_id);
			__tail_le64(buf + off, snap[i].detail); off += 8;
			break;
		}
		if ((rc = __tail_write_all(fd, buf, off)) != XTC_OK) goto out;
	}
out:
	if (snap != NULL) __os_free(snap);
	return rc;
}

/* ------------------------------------------------------------------ *
 * Production deployment: env-driven enable + spill-to-directory.
 *
 * xtc_tail is off by default and lives in an in-memory ring; to run it in
 * a deployed application you (a) turn it on with no code change via the
 * XTC_TAIL_* environment (xtc_tail_from_env), and (b) get a trace off the
 * box by spilling a dial9 segment to a directory (xtc_tail_spill_dial9),
 * which a sidecar or scp then ships to wherever the dial9 viewer reads.
 *
 * Deliberately NO background thread (ponytail: a thread is only warranted
 * once a consumer needs unattended periodic spill).  The caller decides
 * when to spill -- at shutdown, on a signal, on a health-check trigger, or
 * periodically from its own timer.  A crash-surviving trace is the caller
 * spilling from a fault handler; xtc_tail_spill_dial9 only writes and never
 * allocates beyond the one snapshot the dump already takes.
 * ------------------------------------------------------------------ */


/* PUBLIC: unsigned xtc_tail_from_env __P((void)); */
/*
 * Configure xtc_tail from the environment, mirroring dial9's DIAL9_*:
 *   XTC_TAIL_ENABLE -- "1"/"sched"/anything truthy turns on the SCHED
 *                      source; "all" turns on every source; "0"/"off"/
 *                      "false"/unset leaves it disabled.
 * Returns the enabled source mask (0 if disabled).  Zero-code: a deployed
 * binary linked against libxtc records nothing until the operator sets the
 * variable.  Reads through the guarded __os_env_get wrapper.
 */
unsigned
xtc_tail_from_env(void)
{
	char v[32];
	unsigned mask = 0;

	if (__os_env_get("XTC_TAIL_ENABLE", v, sizeof v) == XTC_OK &&
	    v[0] != '\0') {
		if (strcmp(v, "all") == 0)
			mask = XTC_TAIL_ALL;
		else if (strcmp(v, "0") != 0 && strcmp(v, "off") != 0 &&
		         strcmp(v, "false") != 0)
			mask = XTC_TAIL_SCHED;
	}
	if (mask != 0)
		(void)xtc_tail_enable(mask);
	return mask;
}

/* PUBLIC: int xtc_tail_spill_dial9 __P((const char *)); */
/*
 * Write the current ring to a new, uniquely-named dial9 segment file in
 * `dir` (which must exist): xtc-tail-<pid>-<monotonic-ns>.d9, so successive
 * spills never collide and sort by time.  Returns XTC_OK on success.
 *
 * Rotation/byte-budget is the operator's job (a directory plus a find/logrotate
 * rule), matching how dial9's DiskBuffer is ultimately bounded by the
 * deployment; the library side stays simple.  An in-library budget is a clean
 * addition when a consumer needs it.
 */
int
xtc_tail_spill_dial9(const char *dir)
{
	char path[512];
	int fd = -1, rc, n;

	if (dir == NULL) return XTC_E_INVAL;
	n = snprintf(path, sizeof path, "%s/xtc-tail-%ld-%llu.d9",
	    dir, (long)getpid(), (unsigned long long)__tail_now_ns());
	if (n < 0 || (size_t)n >= sizeof path) return XTC_E_INVAL;
	/* Portable open via the OS file wrapper (POSIX open + Win32 _open),
	 * so the spill compiles and runs on every target -- the raw open()
	 * the XTCL dump avoids by taking a caller fd is not portable here. */
	rc = xtc_fs_open(path, XTC_FS_WRITE | XTC_FS_CREATE | XTC_FS_TRUNC, &fd);
	if (rc != XTC_OK) return rc;
	rc = xtc_tail_dump_dial9(fd);
	(void)xtc_fs_close(fd);
	return rc;
}
