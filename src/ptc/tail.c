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
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock */
#include "os_time.h"

#include <pthread.h>
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
