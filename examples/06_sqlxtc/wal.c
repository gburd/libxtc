/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/wal.c
 *	Write-ahead log: a log buffer with parallel insert and group commit
 *	for committers on fibers and plain threads alike.  See the comment
 *	above struct wal for the algorithm and where each part comes from
 *	(Berkeley DB, JE, InnoDB, the PostgreSQL async WAL series).  Writes
 *	and fdatasyncs go through libxtc async file I/O (xtc_aio): native
 *	io_uring completion where the leader runs on a capable loop, a
 *	blocking-pool offload elsewhere, inline off a loop.
 */

#include "wal.h"

#include "xtc_aio.h"
#include "xtc_async.h"    /* xtc_yield */
#include "xtc_proc.h"
#include "xtc_sync.h"     /* xtc_notify: waiters park (fiber) or block (thread) */
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
#if defined(__GNUC__) && defined(__x86_64__)
#include <nmmintrin.h>      /* _mm_crc32_* (SSE4.2, per-function target) */
#elif defined(__GNUC__) && defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>       /* __crc32c* */
#endif

/* The message that stops the writer proc. */
#define WAL_KIND_STOP    'S'

#define WAL_REC_HDR      12u     /* on-disk per-record header: u64 lsn + u32 len */
#define WAL_REC_CRC      8u      /* on-disk per-record trailer: u64 checksum */

/*
 * Per-record on-disk layout is now [u64 lsn][u32 len][body:len][u64 crc].
 * The trailer is a checksum over the header bytes (lsn+len) and the body
 * (a tagged CRC32C, or the 64-bit FNV-1a of logs written before it: see
 * wal_sum_ok).  It makes every record self-checking: recovery can
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

/* 64-bit FNV-1a, continued from h over n bytes.  Byte-at-a-time FNV is
 * a serial multiply chain (~1 byte per 4 cycles), and the reopen scan ran
 * it over every byte of the log: unrolling 8 bytes per iteration lets the
 * compiler schedule the loads and xors ahead of the multiplies.  Same
 * byte order, same result: the on-disk checksum is unchanged. */
#define WAL_FNV_PRIME 1099511628211ull
static uint64_t
wal_fnv(uint64_t h, const uint8_t *p, size_t n)
{
	while (n >= 8) {
		h = (h ^ p[0]) * WAL_FNV_PRIME;
		h = (h ^ p[1]) * WAL_FNV_PRIME;
		h = (h ^ p[2]) * WAL_FNV_PRIME;
		h = (h ^ p[3]) * WAL_FNV_PRIME;
		h = (h ^ p[4]) * WAL_FNV_PRIME;
		h = (h ^ p[5]) * WAL_FNV_PRIME;
		h = (h ^ p[6]) * WAL_FNV_PRIME;
		h = (h ^ p[7]) * WAL_FNV_PRIME;
		p += 8;
		n -= 8;
	}
	while (n-- > 0)
		h = (h ^ *p++) * WAL_FNV_PRIME;
	return h;
}
#define WAL_FNV_BASIS 1469598103934665603ull

/*
 * CRC32C (Castagnoli), the checksum of every frame written since the
 * byte-serial FNV-1a: the TPC-C W=10 replay spent 89% of its time in
 * wal_fnv (1.1 GB/s).  x86-64 SSE4.2 and ARMv8 have a CRC32C instruction
 * (8 bytes per op); elsewhere slice-by-8 tables.  Same value either way
 * (test_wal_crc), so the log bytes never depend on the host.
 *
 * The trailer of a CRC32C frame is (WAL_SUM_TAG << 32) | crc32c(header
 * and body); an FNV frame's trailer is its 64-bit FNV-1a.  The header is
 * unchanged, so the frame length, and every parser of it, is too.  A
 * reader accepts a frame if the tagged CRC32C or the FNV-1a matches
 * (wal_sum_ok): old logs, and logs that a newer build appended to, read
 * frame by frame, and an FNV frame whose hash happens to carry the tag is
 * still accepted.  A torn frame passes only if both 32-bit halves match
 * by chance.  The preallocated zeros still end the log: their trailer is
 * untagged and a zero-length frame's FNV-1a is never 0.  A build older
 * than this one reads the first CRC32C frame as the end of the log.
 */
#define WAL_SUM_TAG 0x43524332u           /* "2CRC" */
#if defined(WAL_C32C_HW)
/* set by the build: test_wal_crc_sw builds with -DWAL_C32C_HW=0 */
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && \
    (defined(__x86_64__) || \
    (defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)))
#define WAL_C32C_HW 1           /* a CRC32C instruction (x86: if SSE4.2) */
#else
#define WAL_C32C_HW 0
#endif
static uint32_t        wal_c32c_tab[8][256];
static pthread_once_t  wal_c32c_once = PTHREAD_ONCE_INIT;
static int             wal_c32c_hw;          /* set once by wal_c32c_init */

static void
wal_c32c_init(void)
{
	uint32_t c;
	int i, k;
	for (i = 0; i < 256; i++) {
		for (c = (uint32_t)i, k = 0; k < 8; k++)
			c = (c >> 1) ^ (c & 1 ? 0x82f63b78u : 0);
		wal_c32c_tab[0][i] = c;
	}
	for (i = 0; i < 256; i++)
		for (k = 1; k < 8; k++)
			wal_c32c_tab[k][i] = (wal_c32c_tab[k - 1][i] >> 8) ^
			    wal_c32c_tab[0][wal_c32c_tab[k - 1][i] & 0xff];
#if WAL_C32C_HW && defined(__x86_64__)
	wal_c32c_hw = __builtin_cpu_supports("sse4.2") ? 1 : 0;
#else
	wal_c32c_hw = WAL_C32C_HW;
#endif
}

/* The CRC register c continued over n bytes (no pre/post inversion). */
static uint32_t
wal_c32c_sw(uint32_t c, const uint8_t *p, size_t n)
{
	uint64_t v;
	while (n >= 8) {
		/* bytes in stream order, whatever the host's byte order */
		v = (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
		    (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 |
		    (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 |
		    (uint64_t)p[7] << 56;
		v ^= c;
		c = wal_c32c_tab[7][v & 0xff] ^ wal_c32c_tab[6][(v >> 8) & 0xff] ^
		    wal_c32c_tab[5][(v >> 16) & 0xff] ^ wal_c32c_tab[4][(v >> 24) & 0xff] ^
		    wal_c32c_tab[3][(v >> 32) & 0xff] ^ wal_c32c_tab[2][(v >> 40) & 0xff] ^
		    wal_c32c_tab[1][(v >> 48) & 0xff] ^ wal_c32c_tab[0][v >> 56];
		p += 8;
		n -= 8;
	}
	while (n-- > 0)
		c = (c >> 8) ^ wal_c32c_tab[0][(c ^ *p++) & 0xff];
	return c;
}

#if WAL_C32C_HW && defined(__x86_64__)
__attribute__((target("sse4.2"))) static uint32_t
wal_c32c_hwfn(uint32_t c, const uint8_t *p, size_t n)
{
	uint64_t v, c64 = c;
	while (n >= 8) {
		memcpy(&v, p, 8);
		c64 = _mm_crc32_u64(c64, v);
		p += 8;
		n -= 8;
	}
	c = (uint32_t)c64;
	while (n-- > 0)
		c = _mm_crc32_u8(c, *p++);
	return c;
}
#elif WAL_C32C_HW
static uint32_t
wal_c32c_hwfn(uint32_t c, const uint8_t *p, size_t n)
{
	uint64_t v;
	while (n >= 8) {
		memcpy(&v, p, 8);
		c = __crc32cd(c, v);
		p += 8;
		n -= 8;
	}
	while (n-- > 0)
		c = __crc32cb(c, *p++);
	return c;
}
#else
#define wal_c32c_hwfn wal_c32c_sw
#endif

static uint32_t
wal_c32c(uint32_t c, const uint8_t *p, size_t n)
{
	(void)pthread_once(&wal_c32c_once, wal_c32c_init);
	return wal_c32c_hw > 0 ? wal_c32c_hwfn(c, p, n) : wal_c32c_sw(c, p, n);
}

/* The trailer of a CRC32C frame from the CRC register over the frame. */
#define WAL_SUM_TRL(c) (((uint64_t)WAL_SUM_TAG << 32) | (uint32_t)~(c))

/* 1 if trl checks the frame f[0, WAL_REC_HDR + len) (header + body). */
static int
wal_sum_ok(uint64_t trl, const uint8_t *f, uint32_t len)
{
	if ((uint32_t)(trl >> 32) == WAL_SUM_TAG &&
	    trl == WAL_SUM_TRL(wal_c32c(~0u, f, WAL_REC_HDR + (size_t)len)))
		return 1;
	return wal_fnv(WAL_FNV_BASIS, f, WAL_REC_HDR + (size_t)len) == trl;
}

/* The trailer of a new frame: CRC32C over the header (lsn+len) and the
 * body.  Not cryptographic -- it only needs to catch a torn/partial tail
 * record, where the trailing 8 bytes are either missing (short read) or
 * do not match a body the writer never finished. */
static uint64_t
wal_rec_crc(uint64_t lsn, uint32_t len, const void *body)
{
	uint8_t hdr[WAL_REC_HDR];

	memcpy(hdr, &lsn, 8);
	memcpy(hdr + 8, &len, 4);
	return WAL_SUM_TRL(wal_c32c(wal_c32c(~0u, hdr, WAL_REC_HDR), body, len));
}

static off_t
wal_fsize(int fd)
{
	off_t e = lseek(fd, 0, SEEK_END);
	return e < 0 ? 0 : e;
}

/*
 * A buffered frame reader for the sequential scans.  The scans used to
 * issue three preads per frame (header, trailer, body in 64 KiB chunks)
 * and a fourth to hand the body to the callback; this keeps a window of
 * the file (WAL_RD_ONE bytes) and serves every frame that fits in it
 * from memory, refilling with one pread when the next frame runs past
 * the window.  A frame larger than the window is verified in chunks
 * straight from the file, so a torn tail whose length field is garbage
 * never drives an allocation.  buf == NULL (no memory) degrades to the
 * chunked path for every frame.
 */
#define WAL_RD_ONE (1u << 20)
struct wal_rd {
	int      fd;
	off_t    fsize;
	uint8_t *buf;                 /* WAL_RD_ONE bytes, or NULL */
	off_t    woff;                /* file offset of buf[0] */
	size_t   wlen;                /* valid bytes in buf */
};

/* The scans read the log front to back: POSIX_FADV_SEQUENTIAL doubles
 * the kernel's read-ahead window for the fd, so the next reads are under
 * way while the scan checksums and replays the window it has (TPC-C
 * W=10 replay, cold: 0.762 s -> 0.677 s; a plain read of the file takes
 * 0.62 s).  Advice only: no byte a read returns changes. */
static void
wal_rd_seq(int fd)
{
#if defined(POSIX_FADV_SEQUENTIAL)
	(void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
	(void)fd;
#endif
}

static void
wal_rd_init(struct wal_rd *r, int fd, int buffered)
{
	r->fd = fd;
	r->fsize = wal_fsize(fd);
	if (buffered)
		wal_rd_seq(fd);
	r->buf = buffered ? malloc(WAL_RD_ONE) : NULL;
	r->woff = 0;
	r->wlen = 0;
}

/* A pointer to file bytes [off, off + n) if they are (or, after one
 * refill, become) inside the window; NULL otherwise. */
static const uint8_t *
wal_rd_at(struct wal_rd *r, off_t off, size_t n)
{
	ssize_t got;
	if (r->buf == NULL || n > WAL_RD_ONE)
		return NULL;
	if (off >= r->woff && (uint64_t)(off - r->woff) + n <= r->wlen)
		return r->buf + (off - r->woff);
	do {
		got = pread(r->fd, r->buf, WAL_RD_ONE, off);
	} while (got < 0 && errno == EINTR);
	r->woff = off;
	r->wlen = got > 0 ? (size_t)got : 0;
	return n <= r->wlen ? r->buf : NULL;
}

/*
 * Verify the frame at `off` of the file without trusting its length
 * field: returns 1 with *lsn / *len when a COMPLETE frame with a
 * matching checksum is there, 0 at the end of the log (EOF, a torn
 * header, body or trailer, or a checksum mismatch).  *body points at the
 * body inside the reader's window when the whole frame is there, else it
 * is NULL (the caller preads the body).  A record body larger than
 * WAL_MAX_REC (a big transaction) is checksummed in chunks -- the scans
 * used to stop at any record over 16 MiB, which LOST an acknowledged big
 * transaction and every commit after it in recovery (test_wal_bigtxn).
 * The length must also fit in the file.
 */
static int
wal_rd_frame(struct wal_rd *r, off_t off, uint64_t *lsn, uint32_t *len,
    const uint8_t **body)
{
	uint8_t hdr[WAL_REC_HDR], trl[WAL_REC_CRC], chunk[64 * 1024];
	const uint8_t *p;
	uint64_t crc, h, fl;
	uint32_t done, c, k;
	int tagged, pass;

	*body = NULL;
	if (off < 0 || off + (off_t)WAL_REC_HDR > r->fsize)
		return 0;
	if ((p = wal_rd_at(r, off, WAL_REC_HDR)) != NULL)
		memcpy(hdr, p, WAL_REC_HDR);
	else if (pread(r->fd, hdr, WAL_REC_HDR, off) != (ssize_t)WAL_REC_HDR)
		return 0;
	memcpy(lsn, hdr, 8);
	memcpy(len, hdr + 8, 4);
	fl = (uint64_t)WAL_REC_HDR + *len + WAL_REC_CRC;
	if ((uint64_t)off + fl > (uint64_t)r->fsize)
		return 0;                    /* runs past the end: torn */
	if ((p = wal_rd_at(r, off, (size_t)fl)) != NULL) {
		memcpy(&crc, p + WAL_REC_HDR + *len, WAL_REC_CRC);
		if (!wal_sum_ok(crc, p, *len))
			return 0;
		*body = p + WAL_REC_HDR;
		return 1;
	}
	if (pread(r->fd, trl, WAL_REC_CRC, off + (off_t)WAL_REC_HDR + (off_t)*len)
	    != (ssize_t)WAL_REC_CRC)
		return 0;
	memcpy(&crc, trl, WAL_REC_CRC);
	/* The tag says which sum to compute; FNV-1a only if CRC32C fails (an
	 * old frame whose hash carries the tag, or a torn one: once a log). */
	tagged = (uint32_t)(crc >> 32) == WAL_SUM_TAG;
	for (pass = tagged ? 0 : 1; pass < 2; pass++) {
		h = wal_fnv(WAL_FNV_BASIS, hdr, WAL_REC_HDR);
		c = wal_c32c(~0u, hdr, WAL_REC_HDR);
		for (done = 0; done < *len; done += k) {
			k = *len - done < sizeof chunk ? *len - done :
			    (uint32_t)sizeof chunk;
			if (pread(r->fd, chunk, k, off + (off_t)WAL_REC_HDR +
			    (off_t)done) != (ssize_t)k)
				return 0;
			if (pass == 0)
				c = wal_c32c(c, chunk, k);
			else
				h = wal_fnv(h, chunk, k);
		}
		if (pass == 0 ? crc == WAL_SUM_TRL(c) : h == crc)
			return 1;
	}
	return 0;
}

/* Verify one frame with no window (the tail-hint check). */
static int
wal_frame_ok(int fd, off_t off, off_t fsize, uint64_t *lsn, uint32_t *len)
{
	struct wal_rd r;
	const uint8_t *body;
	r.fd = fd;
	r.fsize = fsize;
	r.buf = NULL;
	r.woff = 0;
	r.wlen = 0;
	return wal_rd_frame(&r, off, lsn, len, &body);
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

/*
 * ---- the log buffer: parallel insert + group commit ----
 *
 * One path for every committer, fiber or thread (the writer proc and the
 * off-loop mutex path it replaces each copied a record twice and moved it
 * by message or under one lock).  The design follows the engines that got
 * this right:
 *
 *   reserve   under res_mu, a few instructions: the next LSN and the
 *             record's byte range [start, end) in a ring buffer (Berkeley
 *             DB __log_put_next's LSN/offset assignment; JE allocates log
 *             buffer space under the LWL the same way).  If the ring has
 *             no room the committer flushes (or waits for the flush) and
 *             retries.
 *   copy      OUTSIDE any lock: header, body and checksum straight into
 *             the ring (JE: "the client computes the checksum and copies
 *             the data into the log buffer, not holding the LWL").  Then
 *             publish completion in the LSN's slot of a completion ring
 *             (InnoDB's recent_written link buffer, keyed by LSN).
 *   flush     a leader (the first committer to find no flush running --
 *             BDB's in_flush / JE's FSyncManager leader) advances the
 *             contiguous-completed watermark over the completion slots,
 *             pwrites [written, copied) and fdatasyncs it, then publishes
 *             durable (CAS-max, as noxu) and wakes exactly the waiters it
 *             covered plus ONE uncovered waiter as the next leader (BDB
 *             __log_flush_int's commit queue + DB_COMMIT_FLUSH baton), so
 *             there is no thundering herd.  Records that arrive during a
 *             flush form the next batch: the batch self-tunes to the fsync
 *             time, a lone committer pays exactly one fsync.
 *   wait      on an xtc_notify (a fiber parks, a thread blocks), and every
 *             woken waiter re-verifies the durable watermark rather than
 *             trusting the wakeup (BDB's rule).
 *
 * A leader on a fiber writes with xtc_aio (the fiber parks, its loop runs
 * on); off a loop xtc_aio runs inline.  The fail-stop latch is unchanged.
 * The on-disk record format is unchanged.  Ring positions are file
 * offsets, so durable_off is just the durable position.
 */
#define WAL_RING     (8u << 20)     /* log buffer bytes (power of 2) */
#define WAL_SLOTS    (1u << 16)     /* completion slots (power of 2) */

struct wal_slot {                   /* completion of one LSN */
	_Atomic uint64_t lsn;           /* == the record's LSN once copied */
	uint64_t         end;           /* position just past the record */
};

struct wal_waiter {
	uint64_t           need;        /* position that must be durable */
	int                baton;       /* handed the leadership */
	int                fiber;       /* waits on a proc */
	xtc_notify_t      *n;
	struct wal_waiter *next;
};

struct wal {
	int        fd;
	/* The append offset (== res_pos), for wal_size: stored under res_mu,
	 * read with no lock (the checkpoint proc polls it), so atomic. */
	_Atomic uint64_t off;
	int64_t    window_ns;
	uint32_t   max_batch;
	xtc_pid_t  writer_pid;
	_Atomic int writer_alive;    /* writer proc live; cleared on its exit */

	/* Reservation (res_mu): the next LSN and byte position. */
	pthread_mutex_t    res_mu;
	uint64_t           next_lsn;     /* last LSN handed out */
	uint64_t           res_pos;      /* next record starts here */
	int                direct;       /* a big record is being written
	                                  * directly: reservations wait */
	uint8_t           *ring;         /* WAL_RING bytes: positions mod size */
	struct wal_slot   *slot;         /* WAL_SLOTS completion slots */

	/* Flush (fl_mu): the leader flag, the watermarks, the waiter queue. */
	pthread_mutex_t    fl_mu;
	int                flushing;     /* a leader is running */
	uint64_t           pre_end;      /* file is written (zeros) to here:
	                                  * leader-only, see wal_prealloc */
	uint64_t           copied_lsn;   /* every LSN <= this is in the ring */
	uint64_t           copied_pos;   /* ... and ends here */
	_Atomic uint64_t   written;      /* bytes below are pwritten */
	struct wal_waiter *wq;           /* sorted by need (ascending) */
	struct wal_waiter *wfree;        /* recycled waiters (BDB free_commits) */

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

	/* Stats (fl_mu). */
	uint64_t s_commits;
	uint64_t s_batches;
	_Atomic uint64_t s_bytes;
	uint64_t s_maxbatch;
	uint64_t s_failed;            /* commits refused by a failed log write */
};

/* Scan an open log fd to its end: *off_out := offset just past the last
 * COMPLETE record (a torn tail is excluded), *maxlsn_out := highest LSN
 * seen.  Used to resume appending to an existing log. */
static void
wal_scan_tail(int fd, off_t *off_out, uint64_t *maxlsn_out)
{
	struct wal_rd r;
	const uint8_t *body;
	off_t o = 0;
	uint64_t maxlsn = 0, lsn;
	uint32_t len;
	wal_rd_init(&r, fd, 1);
	while (wal_rd_frame(&r, o, &lsn, &len, &body)) {
		if (lsn > maxlsn) maxlsn = lsn;
		o += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
	}
	free(r.buf);
	*off_out = o;
	*maxlsn_out = maxlsn;
}

/*
 * Check a tail hint (wal_opts_t.tail) against the file in O(one frame):
 * the frame at last_off is complete, checksums, ends at end and carries
 * max_lsn, and no complete frame starts at end (nothing was appended
 * after the scan that produced the hint).  1 if it holds.
 */
static int
wal_tail_ok(int fd, const wal_tail_t *t)
{
	off_t fsize = wal_fsize(fd);
	uint64_t lsn;
	uint32_t len;
	if (t == NULL || !t->valid || t->last_off > t->end ||
	    t->end > (uint64_t)fsize)
		return 0;
	if (wal_frame_ok(fd, (off_t)t->end, fsize, &lsn, &len))
		return 0;                    /* the log goes on past the hint */
	if (t->last_off == t->end)       /* an empty log */
		return t->end == 0 && t->max_lsn == 0;
	return wal_frame_ok(fd, (off_t)t->last_off, fsize, &lsn, &len) &&
	    t->last_off + WAL_REC_HDR + len + WAL_REC_CRC == t->end &&
	    lsn == t->max_lsn;
}

/* Point every watermark at (pos, lsn): an empty buffer.  Callers hold no
 * concurrent committers (open, rebind, truncate run quiesced). */
static void
wal_reset_marks(wal_t *w, uint64_t pos, uint64_t lsn)
{
	atomic_store_explicit(&w->off, pos, memory_order_relaxed);
	w->res_pos = pos;
	w->next_lsn = lsn;
	w->copied_pos = pos;
	w->copied_lsn = lsn;
	w->pre_end = pos;            /* the file ends at pos (see callers) */
	atomic_store_explicit(&w->written, pos, memory_order_release);
	atomic_store_explicit(&w->durable_off, pos, memory_order_release);
	atomic_store_explicit(&w->durable_lsn, lsn, memory_order_release);
}

int
wal_open(const wal_opts_t *opts, wal_t **out)
{
	wal_t *w;
	off_t o = 0;
	uint64_t maxlsn = 0;

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
	if (opts->append) {
		/* Continue an existing log: resume LSNs past the highest on
		 * disk, and append after the last COMPLETE record (a torn tail
		 * from a crash mid-append is dropped). */
		int tr;
		if (wal_tail_ok(w->fd, opts->tail)) {
			o = (off_t)opts->tail->end;   /* trusted: no scan */
			maxlsn = opts->tail->max_lsn;
		} else {
			wal_scan_tail(w->fd, &o, &maxlsn);
		}
		tr = ftruncate(w->fd, o);  /* drop torn tail */
		(void)tr;                  /* non-fatal: appends overwrite it */
	}
	w->window_ns = opts->window_ns > 0 ? opts->window_ns : 500000;   /* 0.5ms */
	w->max_batch = opts->max_batch > 0 ? opts->max_batch : 256;
	w->ring = malloc(WAL_RING);
	w->slot = calloc(WAL_SLOTS, sizeof *w->slot);
	if (w->ring == NULL || w->slot == NULL) {
		free(w->ring); free(w->slot); close(w->fd); free(w);
		return XTC_E_NOMEM;
	}
	(void)pthread_mutex_init(&w->res_mu, NULL);
	(void)pthread_mutex_init(&w->fl_mu, NULL);
	wal_reset_marks(w, (uint64_t)o, maxlsn);
	*out = w;
	return XTC_OK;
}

void
wal_close(wal_t *w)
{
	struct wal_waiter *q;
	if (w == NULL)
		return;
	while ((q = w->wfree) != NULL) {
		w->wfree = q->next;
		xtc_notify_destroy(q->n);
		xtc_free(q);
	}
	(void)pthread_mutex_destroy(&w->fl_mu);
	(void)pthread_mutex_destroy(&w->res_mu);
	if (w->fd >= 0)
		close(w->fd);
	free(w->ring);
	free(w->slot);
	free(w);
}

/*
 * res_mu / fl_mu are held only for a few instructions and never across a
 * park, but under involuntary preemption (xtc_preempt_set_involuntary) a
 * fiber could still be preempted while holding one; a loop-mate that
 * then blocks its OS thread on the same mutex can never let the holder
 * run again (test_wal_kill preempt-nokill hung that way with no kill at
 * all).  A proc critical section makes the preemption timer defer for
 * the hold.  Off a proc both calls are no-ops.
 */
static void
wal_lock(pthread_mutex_t *m)
{
	xtc_proc_critical_enter();
	(void)pthread_mutex_lock(m);
}

static void
wal_unlock(pthread_mutex_t *m)
{
	(void)pthread_mutex_unlock(m);
	xtc_proc_critical_leave();
}

/*
 * DST pessimal point: under the simulator with buggify on, yield the
 * fiber here on a seeded coin.  Every site sits in a window a reviewer
 * flagged (the copy between reservation and publication, a leader about
 * to write, a waiter about to park, the direct big-record write): the
 * yield lets peers run there and makes the window a kill delivery point,
 * so test_sim_wal_kill can kill a committer at exactly that moment.  A
 * single relaxed load in production; never called with a lock held.
 */
static void
wal_buggify_yield(const char *site)
{
	if (XTC_SIM_BUGGIFY(site) && xtc_sim_buggify_fault(250) &&
	    !xtc_pid_is_none(xtc_self()))
		xtc_yield();
}

/* Copy n bytes to ring position pos (wrapping). */
static void
ring_put(wal_t *w, uint64_t pos, const void *src, size_t n)
{
	size_t at = (size_t)(pos & (WAL_RING - 1)), k = WAL_RING - at;
	if (n <= k) {
		memcpy(w->ring + at, src, n);
	} else {
		memcpy(w->ring + at, src, k);
		memcpy(w->ring, (const uint8_t *)src + k, n - k);
	}
}

/* fl_mu held: advance the contiguous-completed watermark. */
static void
wal_advance_copied(wal_t *w)
{
	for (;;) {
		uint64_t want = w->copied_lsn + 1;
		struct wal_slot *s = &w->slot[want & (WAL_SLOTS - 1)];
		if (atomic_load_explicit(&s->lsn, memory_order_acquire) != want)
			return;
		w->copied_lsn = want;
		w->copied_pos = s->end;
	}
}

/* fl_mu held on entry and exit: wake every waiter `durable` covers and,
 * if any waiter is left, hand ONE of them the leadership.
 *
 * The waiters are unlinked under fl_mu and signalled after it is
 * dropped.  Signalling every covered waiter under fl_mu (a thread's
 * signal is a futex wake, ~3 us), the baton last, kept the log idle for
 * most of each round: at 64 threads the gap between one fdatasync and the
 * next pwrite was 68 us against 41 us of I/O.  Order, measured with
 * bench_wal: fiber waiters and the first WAL_WAKE_FIRST thread waiters
 * before the baton (they re-append in time to join the next batch: the
 * baton first lost 15% at 4 threads and 10% at 64 fibers), then the
 * baton, then the other threads, whose wakes no longer delay the next
 * flush (32 threads: 282k commits/s against 125k).  An unlinked waiter is
 * touched only by its own committer once signalled, so the lists are
 * complete before the first signal. */
#define WAL_WAKE_FIRST 2
static void
wal_wake_locked(wal_t *w, uint64_t durable, int give_baton)
{
	struct wal_waiter *q, *now = NULL, *later = NULL, *bat = NULL;
	struct wal_waiter **nt = &now, **lt = &later;
	int k = 0;
	while ((q = w->wq) != NULL && q->need <= durable) {
		w->wq = q->next;
		q->next = NULL;
		if (q->fiber || k++ < WAL_WAKE_FIRST) {
			*nt = q;
			nt = &q->next;
		} else {
			*lt = q;
			lt = &q->next;
		}
	}
	if (give_baton && w->wq != NULL && !w->flushing) {
		bat = w->wq;
		bat->baton = 1;
		w->flushing = 1;             /* counted for the baton holder */
		w->wq = bat->next;
	}
	if (now == NULL && bat == NULL && later == NULL)
		return;
	wal_unlock(&w->fl_mu);
	while ((q = now) != NULL) {
		now = q->next;
		(void)xtc_notify_signal(q->n);
	}
	if (bat != NULL)
		(void)xtc_notify_signal(bat->n);
	while ((q = later) != NULL) {
		later = q->next;
		(void)xtc_notify_signal(q->n);
	}
	wal_lock(&w->fl_mu);
}

/*
 * Keep the file written with zeros at least WAL_PRE_AHEAD past `upto`,
 * WAL_PRE_CHUNK at a time.  The next appends then OVERWRITE written
 * blocks inside the file instead of growing it, so a commit's fdatasync
 * flushes data only -- no size or extent metadata to journal.  Measured
 * on this XFS NVMe, 300-byte commits: append+fdatasync 104 us, the same
 * into a pre-written file 34 us (an fallocate'd, unwritten range: 43 us,
 * 120 us at 4 KiB -- it still converts extents, so real zeros).
 * WiredTiger preallocates its log (__log_prealloc), InnoDB's redo files
 * are fixed size.  Zeros end the log for every scan: a zero frame has
 * length 0 and checksum 0, and a length-0 record's checksum is never 0
 * (FNV-1a of 12 bytes from an odd basis is odd).
 *
 * Leader-only (w->flushing held, fl_mu NOT held): normal records reach
 * the file only through the leader, and the direct writer takes the
 * leadership too, so nothing else writes past `upto` meanwhile.  The
 * fdatasync that follows also makes the zeros durable (once a chunk).
 * Best-effort: a failed extension leaves the log appending, as before.
 */
#define WAL_PRE_CHUNK (8u << 20)
#define WAL_PRE_AHEAD (4u << 20)
static void
wal_prealloc(wal_t *w, uint64_t upto)
{
	static const uint8_t zeros[1u << 20];
	uint64_t at, end;
	if (upto + WAL_PRE_AHEAD <= w->pre_end)
		return;
	at = w->pre_end > upto ? w->pre_end : upto;
	end = at + WAL_PRE_CHUNK;
	while (at < end) {
		if (xtc_aio_pwrite(w->fd, zeros, (uint32_t)sizeof zeros,
		    (int64_t)at) != (int)sizeof zeros)
			return;
		at += sizeof zeros;
		w->pre_end = at;
	}
}

/* fl_mu held on entry and exit, w->flushing set by the caller: write
 * and fdatasync the contiguous completed prefix, up to at least `need`
 * (the leader's own record, which its own copy has completed). */
static void
wal_flush_locked(wal_t *w, uint64_t need)
{
	uint64_t from, upto, lsn_upto;
	uint32_t nrec;
	int ok = 1, spins = 0;
	for (;;) {
		wal_advance_copied(w);
		if (w->copied_pos >= need)
			break;
		/* A record before ours is mid-copy (a memcpy: it never parks):
		 * wait for it outside the lock. */
		wal_unlock(&w->fl_mu);
		if (++spins > 64) {
			if (xtc_pid_is_none(xtc_self())) (void)xtc_sleep_ns(1000);
			else xtc_yield();
		}
		wal_lock(&w->fl_mu);
	}
	from = atomic_load_explicit(&w->written, memory_order_relaxed);
	upto = w->copied_pos;
	lsn_upto = w->copied_lsn;
	/* Buggify: flush only up to our own record instead of the whole
	 * completed prefix -- more, smaller batches and fsync boundaries for
	 * crash recovery to cut at (legal: no batching promise is made). */
	if (XTC_SIM_BUGGIFY("wal.flush.tiny_batch") && xtc_sim_buggify_fault(300) &&
	    need > from && need < upto) {
		uint64_t l = w->copied_lsn;
		while (l > atomic_load_explicit(&w->durable_lsn, memory_order_relaxed) &&
		    w->slot[l & (WAL_SLOTS - 1)].end > need)
			l--;
		upto = w->slot[l & (WAL_SLOTS - 1)].end;
		lsn_upto = l;
	}
	nrec = (uint32_t)(lsn_upto - atomic_load_explicit(&w->durable_lsn,
	    memory_order_relaxed));
	wal_unlock(&w->fl_mu);
	wal_buggify_yield("wal.flush.leader_yield");

	if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
		ok = 0;
	} else if (upto > atomic_load_explicit(&w->durable_off, memory_order_relaxed)) {
		size_t at = (size_t)(from & (WAL_RING - 1));
		size_t n = (size_t)(upto - from), k = WAL_RING - at;
		wal_prealloc(w, upto);
		if (n == 0) {
			ok = 1;               /* written directly; only sync it */
		} else if (n <= k) {
			ok = xtc_aio_pwrite(w->fd, w->ring + at, (uint32_t)n,
			    (int64_t)from) == (int)n;
		} else {
			ok = xtc_aio_pwrite(w->fd, w->ring + at, (uint32_t)k,
			    (int64_t)from) == (int)k &&
			    xtc_aio_pwrite(w->fd, w->ring, (uint32_t)(n - k),
			    (int64_t)(from + k)) == (int)(n - k);
		}
#if XTC_DST_BUG(XTC_DST_BUG_NODURABLE)
		/* planted bug: skip the fdatasync but still report durable */
#else
		if (ok && xtc_aio_fdatasync(w->fd) != 0)
			ok = 0;
#endif
	}

	wal_lock(&w->fl_mu);
	if (ok) {
		atomic_store_explicit(&w->written, upto, memory_order_release);
		if (upto > atomic_load_explicit(&w->durable_off, memory_order_relaxed)) {
			atomic_store_explicit(&w->durable_off, upto, memory_order_release);
			atomic_store_explicit(&w->durable_lsn, lsn_upto, memory_order_release);
		}
		w->s_batches++;
		w->s_commits += nrec;
		if (nrec > w->s_maxbatch) w->s_maxbatch = nrec;
	} else {
		atomic_store_explicit(&w->failed, 1, memory_order_release);
		w->s_failed += nrec;
	}
}

/* Wait (fl_mu held; returns with it held) until `need` is durable, the
 * log fails, or this committer is handed the leadership. */
static void
wal_wait_locked(wal_t *w, uint64_t need)
{
	struct wal_waiter *q = w->wfree, **pp;
	if (q != NULL) {
		w->wfree = q->next;
	} else {
		/* xtc_calloc: the preemption-safe allocator (a fiber here
		 * holds no lock, but could be preempted inside glibc's) */
		q = xtc_calloc(1, sizeof *q);
		if (q == NULL || xtc_notify_create(&q->n) != XTC_OK) {
			/* no waiter object: poll instead */
			xtc_free(q);
			wal_unlock(&w->fl_mu);
			if (xtc_pid_is_none(xtc_self())) (void)xtc_sleep_ns(20000);
			else (void)xtc_proc_sleep(20000);
			wal_lock(&w->fl_mu);
			return;
		}
	}
	q->need = need;
	q->baton = 0;
	q->fiber = !xtc_pid_is_none(xtc_self());
	for (pp = &w->wq; *pp != NULL && (*pp)->need <= need; pp = &(*pp)->next)
		;
	q->next = *pp;
	*pp = q;
	wal_unlock(&w->fl_mu);
	wal_buggify_yield("wal.wait.queued_yield");
	(void)xtc_notify_wait(q->n, -1);
	wal_lock(&w->fl_mu);
	/* A covered waiter was unlinked by the waker; a baton holder too.
	 * Unlink defensively (a stored signal from an earlier round). */
	for (pp = &w->wq; *pp != NULL; pp = &(*pp)->next)
		if (*pp == q) { *pp = q->next; break; }
	if (q->baton) {
		q->baton = 0;
		w->flushing = 0;             /* the caller re-claims it below */
	}
	q->next = w->wfree;
	w->wfree = q;
}

/* fl_mu held: make `need` durable -- lead a flush, or wait for one. */
static int
wal_durable_locked(wal_t *w, uint64_t need)
{
	for (;;) {
		if (atomic_load_explicit(&w->durable_off, memory_order_acquire) >= need) {
			/* Covered.  A baton holder that turns out to be covered
			 * (noxu's handoff) passes the leadership on, so the
			 * waiters behind it are never left without a leader. */
			if (!w->flushing && w->wq != NULL)
				wal_wake_locked(w, atomic_load_explicit(&w->durable_off,
				    memory_order_relaxed), 1);
			return XTC_OK;
		}
		if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
			wal_wake_locked(w, UINT64_MAX, 0);   /* fail them all */
			return XTC_E_IO;
		}
		if (!w->flushing) {
			w->flushing = 1;
			wal_flush_locked(w, need);
			w->flushing = 0;
			wal_wake_locked(w, atomic_load_explicit(&w->failed,
			    memory_order_acquire) ? UINT64_MAX :
			    atomic_load_explicit(&w->durable_off, memory_order_relaxed), 1);
			continue;
		}
		wal_wait_locked(w, need);
	}
}

/* Sleep briefly: park a fiber, block a thread. */
static void
wal_nap(void)
{
	if (xtc_pid_is_none(xtc_self()))
		(void)xtc_sleep_ns(20LL * 1000);
	else
		(void)xtc_proc_sleep(20LL * 1000);
}

/*
 * A record bigger than a quarter of the log buffer (a bulk transaction)
 * is written straight to the file, as JE writes an oversized entry and
 * WiredTiger an "unbuffered" one.  The `direct` flag (under res_mu) holds
 * off new reservations -- they nap and retry rather than block, because
 * the writer parks (its own flush, xtc_aio) and a fiber blocked on a
 * pthread mutex would stall the loop that parked fiber needs.  Then:
 * make everything reserved before it durable, reserve its range, pwrite
 * it, and publish it like a copied record, so a leader fdatasyncs it with
 * nothing left to write for it.  Rare (bulk loads), so serializing the log
 * behind it is fine.
 */
static uint64_t
wal_append_direct(wal_t *w, const void *record, uint32_t len, uint64_t *end,
    int *rc)
{
	uint64_t need = (uint64_t)WAL_REC_HDR + len + WAL_REC_CRC, start, lsn, crc;
	uint64_t target;
	uint8_t hdr[WAL_REC_HDR], trl[WAL_REC_CRC];
	struct wal_slot *s;
	uint32_t done;
	int ok;

	for (;;) {                       /* one direct writer at a time */
		wal_lock(&w->res_mu);
		if (!w->direct)
			break;
		wal_unlock(&w->res_mu);
		wal_nap();
	}
	w->direct = 1;
	target = w->res_pos;
	wal_unlock(&w->res_mu);

	/* Everything before it durable, then take the leadership: no other
	 * leader may write (or zero-fill, wal_prealloc) while this pwrites.
	 * Nothing new is reserved meanwhile, so a running leader has only
	 * records below target left and soon lets go. */
	wal_lock(&w->fl_mu);
	*rc = wal_durable_locked(w, target);
	while (*rc == XTC_OK && w->flushing) {
		wal_unlock(&w->fl_mu);
		wal_nap();
		wal_lock(&w->fl_mu);
	}
	if (*rc == XTC_OK)
		w->flushing = 1;
	wal_unlock(&w->fl_mu);
	wal_buggify_yield("wal.direct.leader_yield");
	wal_lock(&w->res_mu);
	if (*rc != XTC_OK) {
		w->direct = 0;
		wal_unlock(&w->res_mu);
		return 0;
	}
	lsn = ++w->next_lsn;
	start = w->res_pos;              /* == target: nobody reserved since */
	w->res_pos = start + need;
	atomic_store_explicit(&w->off, w->res_pos, memory_order_relaxed);
	wal_unlock(&w->res_mu);

	memcpy(hdr, &lsn, 8);
	memcpy(hdr + 8, &len, 4);
	crc = wal_rec_crc(lsn, len, record);
	memcpy(trl, &crc, WAL_REC_CRC);
	ok = xtc_aio_pwrite(w->fd, hdr, WAL_REC_HDR, (int64_t)start) == (int)WAL_REC_HDR;
	for (done = 0; ok && done < len; ) {
		uint32_t k = len - done < (1u << 24) ? len - done : (1u << 24);
		ok = xtc_aio_pwrite(w->fd, (const uint8_t *)record + done, k,
		    (int64_t)(start + WAL_REC_HDR + done)) == (int)k;
		done += k;
	}
	ok = ok && xtc_aio_pwrite(w->fd, trl, WAL_REC_CRC,
	    (int64_t)(start + WAL_REC_HDR + len)) == (int)WAL_REC_CRC;
	if (start + need > w->pre_end)
		w->pre_end = start + need;   /* the record grew the file */

	wal_lock(&w->fl_mu);
	if (ok) {
		/* in the page cache: the next leader only needs to sync it */
		atomic_store_explicit(&w->written, start + need, memory_order_release);
		s = &w->slot[lsn & (WAL_SLOTS - 1)];
		s->end = start + need;
		atomic_store_explicit(&s->lsn, lsn, memory_order_release);
		atomic_fetch_add_explicit(&w->s_bytes, len, memory_order_relaxed);
	} else {
		/* fail-stop: the reserved range is never published, so no LSN
		 * after it can become durable either */
		atomic_store_explicit(&w->failed, 1, memory_order_release);
		w->s_failed++;
	}
	w->flushing = 0;
	wal_wake_locked(w, ok ? atomic_load_explicit(&w->durable_off,
	    memory_order_relaxed) : UINT64_MAX, 1);
	wal_unlock(&w->fl_mu);
	wal_lock(&w->res_mu);
	w->direct = 0;
	wal_unlock(&w->res_mu);
	if (!ok) {
		*rc = XTC_E_IO;
		return 0;
	}
	*end = start + need;
	*rc = XTC_OK;
	return lsn;
}

/* Reserve room for one record and copy it in.  Returns its LSN and end
 * position, or 0 (*rc set) on failure.  Never holds a lock while copying. */
static uint64_t
wal_append(wal_t *w, const void *record, uint32_t len, uint64_t *end, int *rc)
{
	uint64_t need = (uint64_t)WAL_REC_HDR + len + WAL_REC_CRC, start, lsn, crc;
	uint8_t hdr[WAL_REC_HDR], trl[WAL_REC_CRC];
	struct wal_slot *s;

	if (need > WAL_RING / 4)
		return wal_append_direct(w, record, len, end, rc);
	for (;;) {
		wal_lock(&w->res_mu);
		if (atomic_load_explicit(&w->failed, memory_order_acquire)) {
			wal_unlock(&w->res_mu);
			*rc = XTC_E_IO;
			return 0;
		}
		if (w->direct) {             /* a big record is being written */
			wal_unlock(&w->res_mu);
			wal_nap();
			continue;
		}
		/* Room in the ring (the oldest unwritten byte must not be
		 * overwritten) and a free completion slot for the LSN. */
		if (w->res_pos + need - atomic_load_explicit(&w->written,
		    memory_order_acquire) <= WAL_RING &&
		    w->next_lsn + 1 - atomic_load_explicit(&w->durable_lsn,
		    memory_order_acquire) < WAL_SLOTS)
			break;
		{
			uint64_t target = w->res_pos;
			wal_unlock(&w->res_mu);
			/* Full: make what is reserved so far durable, then retry. */
			wal_lock(&w->fl_mu);
			*rc = wal_durable_locked(w, target);
			wal_unlock(&w->fl_mu);
			if (*rc != XTC_OK)
				return 0;
		}
	}
	lsn = ++w->next_lsn;
	start = w->res_pos;
	w->res_pos += need;
	atomic_store_explicit(&w->off, w->res_pos, memory_order_relaxed);
	wal_unlock(&w->res_mu);

	memcpy(hdr, &lsn, 8);
	memcpy(hdr + 8, &len, 4);
	crc = wal_rec_crc(lsn, len, record);
	memcpy(trl, &crc, WAL_REC_CRC);
	ring_put(w, start, hdr, WAL_REC_HDR);
	wal_buggify_yield("wal.append.copy_yield");   /* reserved, unpublished */
	if (len)
		ring_put(w, start + WAL_REC_HDR, record, len);
	ring_put(w, start + WAL_REC_HDR + len, trl, WAL_REC_CRC);
	s = &w->slot[lsn & (WAL_SLOTS - 1)];
	s->end = start + need;
	atomic_store_explicit(&s->lsn, lsn, memory_order_release);   /* publish */
	atomic_fetch_add_explicit(&w->s_bytes, len, memory_order_relaxed);
	*end = start + need;
	*rc = XTC_OK;
	return lsn;
}

static int
wal_commit_any(wal_t *w, const void *record, uint32_t len, uint64_t *lsn)
{
	uint64_t my_lsn, end = 0;
	int rc;

	if (w == NULL || (record == NULL && len != 0))
		return XTC_E_INVAL;
	/*
	 * A kill (xtc_exit_pid, a linked exit) raised at a park point below
	 * -- the waiter queue, the leader's yield or aio, a nap -- would
	 * unwind this fiber with its waiter still queued (a leader later
	 * hands that dead waiter the leadership), with the leadership or the
	 * direct flag held, or with its reserved range never published; the
	 * next flush then waits forever (test_wal_kill).  So the commit runs
	 * with cancellation masked and the kill is delivered by
	 * xtc_mask_leave once the record is durable or failed.  Off a proc
	 * both calls are no-ops.
	 */
#if !XTC_DST_BUG(XTC_DST_BUG_WALKILL)       /* planted bug: unmasked */
	(void)xtc_mask_enter();
#endif
	if ((my_lsn = wal_append(w, record, len, &end, &rc)) != 0) {
		wal_lock(&w->fl_mu);
		rc = wal_durable_locked(w, end);
		wal_unlock(&w->fl_mu);
	}
#if !XTC_DST_BUG(XTC_DST_BUG_WALKILL)
	(void)xtc_mask_leave();      /* may not return: a latched kill */
#endif
	if (rc == XTC_OK && lsn != NULL)
		*lsn = my_lsn;
	return rc;
}

/*
 * The writer proc no longer carries the log: every committer appends and
 * group-commits itself.  It is kept as a proc a caller can spawn and stop
 * (tests and the engine do), flushing whatever is reserved but not yet
 * durable when it is told to stop.
 */
/* At-exit hook of the writer proc: runs on a normal return AND when the
 * proc is killed (xtc_exit_pid, a link), so wal_writer_stop never waits
 * out its timeout for a writer that is already gone. */
static void
wal_writer_gone(void *arg)
{
	wal_t *w = arg;
	atomic_store_explicit(&w->writer_alive, 0, memory_order_release);
}

static void
wal_writer_proc(void *arg)
{
	wal_t *w = arg;
	if (xtc_proc_at_exit(wal_writer_gone, w) != XTC_OK)
		wal_writer_gone(w);          /* no hook: stop must not wait */
	for (;;) {
		void *m = NULL;
		size_t n = 0;
		if (xtc_recv(&m, &n, -1) != XTC_OK || m == NULL)
			continue;
		if (n >= 1 && ((const uint8_t *)m)[0] == WAL_KIND_STOP) {
			free(m);
			break;
		}
		free(m);
	}
	(void)xtc_mask_enter();          /* as wal_commit_any: no torn flush */
	wal_lock(&w->fl_mu);
	(void)wal_durable_locked(w, w->res_pos);
	wal_unlock(&w->fl_mu);
	(void)xtc_mask_leave();
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
	return wal_commit_any(w, record, len, lsn);
}

int
wal_commit_sync(wal_t *w, const void *record, uint32_t len, uint64_t *lsn)
{
	return wal_commit_any(w, record, len, lsn);
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
	wal_reset_marks(w, (uint64_t)off, lsn);
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
	if (k->src_fd >= 0)
		wal_rd_seq(k->src_fd);
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
			/* durable frames below `end`: a length past it is corrupt */
			if ((off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC >
			    end - (k->src_off + (off_t)pos))
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
			if (!wal_sum_ok(crc, k->rbuf + pos, len))
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
wal_scan_ex(const char *path, wal_replay_cb cb, void *user, wal_tail_t *tail)
{
	struct wal_rd r;
	int fd, rc = XTC_OK, stopped = 0;
	off_t off = 0, last = 0;
	uint8_t *buf = NULL;
	size_t bufcap = 0;
	uint64_t maxlsn = 0;

	if (tail != NULL)
		memset(tail, 0, sizeof *tail);
	if (path == NULL || cb == NULL)
		return XTC_E_INVAL;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return XTC_OK;                /* no log yet: nothing to replay */
	wal_rd_init(&r, fd, 1);
	for (;;) {
		uint64_t lsn;
		uint32_t len;
		const uint8_t *body;
		/* Verified before anything is allocated (wal_rd_frame). */
		if (!wal_rd_frame(&r, off, &lsn, &len, &body))
			break;                   /* the end, or a torn tail */
		if (body == NULL) {
			if (len > bufcap) {
				uint8_t *nb = realloc(buf, len);
				if (nb == NULL) { rc = XTC_E_NOMEM; break; }
				buf = nb; bufcap = len;
			}
			if (len > 0 && pread(fd, buf, len, off + (off_t)WAL_REC_HDR) !=
			    (ssize_t)len) {
				stopped = 1;          /* as before: end the replay */
				break;
			}
			body = buf;
		}
		if (lsn > maxlsn)
			maxlsn = lsn;
		last = off;
		off += (off_t)WAL_REC_HDR + (off_t)len + (off_t)WAL_REC_CRC;
		if (cb(lsn, body, len, user) != 0) {
			stopped = 1;
			break;
		}
	}
	if (tail != NULL && rc == XTC_OK && !stopped) {
		tail->valid = 1;
		tail->end = (uint64_t)off;
		tail->last_off = (uint64_t)(off == 0 ? 0 : last);
		tail->max_lsn = maxlsn;
	}
	free(r.buf);
	free(buf);
	close(fd);
	return rc;
}

int
wal_scan(const char *path, wal_replay_cb cb, void *user)
{
	return wal_scan_ex(path, cb, user, NULL);
}

int
wal_truncate(wal_t *w)
{
	if (w == NULL)
		return XTC_E_INVAL;
	wal_lock(&w->res_mu);
	wal_lock(&w->fl_mu);
	if (ftruncate(w->fd, 0) != 0) {
		wal_unlock(&w->fl_mu);
		wal_unlock(&w->res_mu);
		return XTC_E_INTERNAL;
	}
	/* next append at offset 0; next_lsn keeps rising */
	wal_reset_marks(w, 0, w->next_lsn);
	atomic_fetch_add_explicit(&w->gen, 1, memory_order_acq_rel);
	wal_unlock(&w->fl_mu);
	wal_unlock(&w->res_mu);
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
	return w == NULL ? 0 :
	    atomic_load_explicit(&w->off, memory_order_relaxed);
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
	/* the counters are written under fl_mu (by the flush leader) */
	wal_lock(&w->fl_mu);
	out->commits = w->s_commits;
	out->batches = w->s_batches;
	out->max_batch_seen = w->s_maxbatch;
	out->failed = w->s_failed;
	wal_unlock(&w->fl_mu);
	out->bytes = atomic_load_explicit(&w->s_bytes, memory_order_relaxed);
	out->durable_lsn = atomic_load_explicit(&w->durable_lsn, memory_order_relaxed);
}
