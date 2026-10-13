/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/partition.c -- partition log core.
 *
 *	RAM window: a vector of offset slots, each a malloc'd copy of one
 *	record's key+value.  In memory mode it holds every record; with a
 *	directory it holds only the newest records (the active segment and
 *	the one before it) and always ends at the high-water mark
 *	(base + count == hw).
 *
 *	Segments: <dir>/<20-digit-base-offset>.log, records framed as
 *	  crc32 u32 | offset u64 | key_len u32 | value_len u32 | key | value
 *	(big-endian; the crc covers everything after it).  The bytes after
 *	the crc are exactly the FETCH wire encoding of the record, so a
 *	cold read is a pread plus a 4-byte strip.  Each segment keeps a
 *	sparse in-memory index (offset -> file position every IDX_INTERVAL
 *	bytes) built while appending and while scanning at open, so a read
 *	of any offset costs one binary search plus at most IDX_INTERVAL
 *	bytes of scan.  Reads and syncs go through xtc_aio, so the owning
 *	proc parks and the loop keeps running other procs meanwhile.
 *
 *	A segment rolls by syncing the old file, creating the new one and
 *	syncing the directory, so every closed segment is complete and
 *	durable.  Recovery therefore only CRC-checks the newest segment
 *	and truncates its torn tail; older segments are checked
 *	structurally (contiguous offsets, complete records) and verified
 *	by CRC as they are read.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "partition.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xtc_aio.h"

#define SEG_DEFAULT_ROLL   (1u * 1024u * 1024u)   /* 1 MiB */
#define REC_MAX            (16u * 1024u * 1024u)
#define REC_HDR            (4 + 8 + 4 + 4)
#define WIRE_HDR           (8 + 4 + 4)
#define IDX_INTERVAL       4096u                  /* bytes between entries */
#define READ_CHUNK         (256u * 1024u)
#define WBUF_FLUSH         (1u * 1024u * 1024u)   /* write-behind bound */

struct slot {
	uint8_t *bytes;       /* key||value, malloc'd */
	uint32_t key_len;
	uint32_t value_len;
};

struct idx_ent {
	uint64_t off;         /* record offset */
	uint64_t pos;         /* its file position */
};

struct seg {
	uint64_t        base;     /* offset of the first record */
	uint64_t        next;     /* one past the last record */
	uint64_t        size;     /* bytes of complete records */
	struct idx_ent *idx;
	size_t          n_idx, cap_idx;
};

struct plog {
	struct slot *slots;
	uint64_t     base;       /* offset of slots[0] */
	uint64_t     count;      /* slots in use */
	uint64_t     cap;
	uint64_t     mem_bytes;  /* key+value bytes held in slots */
	uint64_t     hw;         /* next offset to assign */
	uint64_t     synced;     /* every offset below this is durable */

	char        *dir;        /* NULL = memory mode */
	struct seg  *segs;       /* ascending base; last is active */
	size_t       n_segs, cap_segs;
	uint64_t     total;      /* bytes across all segments */
	int          fd;         /* active segment, -1 if none */
	uint64_t     wpos;       /* file position of wbuf[0] */
	size_t       wbuf_len;   /* framed bytes not yet written */
	size_t       roll;
	uint64_t     retention;  /* 0 = keep everything */
	int          failed;

	int          rfd;        /* cached read fd */
	uint64_t     rfd_base;   /* segment base it belongs to */
	uint8_t     *scratch;    /* cold-read chunk buffer */
	size_t       scratch_cap;
	uint8_t     *cold;       /* plog_read's returned record */
	size_t       cold_cap;
	uint8_t     *wbuf;       /* framed records awaiting one pwrite */
	size_t       wbuf_cap;
};

static int (*g_sync_fn)(int fd);

void
plog_set_sync_hook(int (*fn)(int fd))
{
	g_sync_fn = fn;
}

static int
__data_sync(int fd)
{
	return g_sync_fn != NULL ? g_sync_fn(fd) : xtc_aio_fdatasync(fd);
}

/* ---- CRC32 (IEEE 802.3), same table as the rexis Bitcask ---- */

static uint32_t __crc_table[256];
static int      __crc_inited;

static void
crc_init(void)
{
	uint32_t i, j, c;
	if (__crc_inited) return;
	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		__crc_table[i] = c;
	}
	__crc_inited = 1;
}

static uint32_t
crc32(const void *data, size_t len, uint32_t init)
{
	const uint8_t *p = data;
	uint32_t c = init ^ 0xffffffffu;
	size_t i;
	if (!__crc_inited) crc_init();
	for (i = 0; i < len; i++)
		c = __crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

static int
__grow(uint8_t **buf, size_t *cap, size_t need)
{
	uint8_t *nb;
	if (need <= *cap) return 0;
	nb = realloc(*buf, need);
	if (nb == NULL) return -1;
	*buf = nb;
	*cap = need;
	return 0;
}

/* ---- RAM window ---- */

static int
__mem_append(plog_t *l, const kaka_record_t *rec)
{
	struct slot *s;
	size_t total = (size_t)rec->key_len + rec->value_len;

	if (l->count == l->cap) {
		uint64_t ncap = l->cap * 2;
		struct slot *ns = realloc(l->slots, (size_t)ncap * sizeof *ns);
		if (ns == NULL) return -1;
		l->slots = ns;
		l->cap = ncap;
	}
	s = &l->slots[l->count];
	s->bytes = NULL;
	if (total > 0 && (s->bytes = malloc(total)) == NULL)
		return -1;
	if (rec->key_len)
		memcpy(s->bytes, rec->key, rec->key_len);
	if (rec->value_len)
		memcpy(s->bytes + rec->key_len, rec->value, rec->value_len);
	s->key_len = rec->key_len;
	s->value_len = rec->value_len;
	l->count++;
	l->mem_bytes += total;
	return 0;
}

/* Drop RAM copies of every record below `floor` (they stay on disk). */
static void
__evict_below(plog_t *l, uint64_t floor)
{
	uint64_t drop, i;
	if (l->dir == NULL || floor <= l->base)
		return;
	drop = floor - l->base;
	if (drop > l->count)
		drop = l->count;
	for (i = 0; i < drop; i++) {
		l->mem_bytes -= (uint64_t)l->slots[i].key_len +
		    l->slots[i].value_len;
		free(l->slots[i].bytes);
	}
	memmove(l->slots, l->slots + drop,
	    (size_t)(l->count - drop) * sizeof *l->slots);
	l->count -= drop;
	l->base += drop;
	if (l->count == 0)
		l->base = floor;
}

static int __flush_wbuf(plog_t *l);

void
plog_evict_all(plog_t *l)
{
	/* Evicted records are read back from the file, so they must be
	 * written first. */
	if (l == NULL || l->failed) return;
	if (l->dir != NULL && __flush_wbuf(l) != 0) {
		l->failed = 1;
		return;
	}
	__evict_below(l, l->hw);
}

/* ---- segments ---- */

static void
__seg_path(char *buf, size_t cap, const char *dir, uint64_t base)
{
	snprintf(buf, cap, "%s/%020llu.log", dir, (unsigned long long)base);
}

static int
__dir_sync(const plog_t *l)
{
	int fd, rc;
	fd = open(l->dir, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return -1;
	rc = xtc_aio_fsync(fd);
	(void)close(fd);
	return rc == 0 ? 0 : -1;
}

static struct seg *
__seg_push(plog_t *l, uint64_t base)
{
	struct seg *s;
	if (l->n_segs == l->cap_segs) {
		size_t nc = l->cap_segs ? l->cap_segs * 2 : 8;
		struct seg *ns = realloc(l->segs, nc * sizeof *ns);
		if (ns == NULL) return NULL;
		l->segs = ns;
		l->cap_segs = nc;
	}
	s = &l->segs[l->n_segs++];
	memset(s, 0, sizeof *s);
	s->base = s->next = base;
	return s;
}

static int
__idx_add(struct seg *s, uint64_t off, uint64_t pos)
{
	if (s->n_idx > 0 && pos - s->idx[s->n_idx - 1].pos < IDX_INTERVAL)
		return 0;
	if (s->n_idx == s->cap_idx) {
		size_t nc = s->cap_idx ? s->cap_idx * 2 : 16;
		struct idx_ent *ni = realloc(s->idx, nc * sizeof *ni);
		if (ni == NULL) return -1;
		s->idx = ni;
		s->cap_idx = nc;
	}
	s->idx[s->n_idx].off = off;
	s->idx[s->n_idx].pos = pos;
	s->n_idx++;
	return 0;
}

/* The segment holding `off`, or NULL. */
static struct seg *
__seg_find(plog_t *l, uint64_t off)
{
	size_t lo = 0, hi = l->n_segs;
	while (hi - lo > 1) {
		size_t mid = lo + (hi - lo) / 2;
		if (l->segs[mid].base <= off) lo = mid; else hi = mid;
	}
	if (l->n_segs == 0 || off < l->segs[lo].base || off >= l->segs[lo].next)
		return NULL;
	return &l->segs[lo];
}

/* Greatest index entry at or below `off` (the first entry is always the
 * segment's first record). */
static const struct idx_ent *
__idx_find(const struct seg *s, uint64_t off)
{
	size_t lo = 0, hi = s->n_idx;
	while (hi - lo > 1) {
		size_t mid = lo + (hi - lo) / 2;
		if (s->idx[mid].off <= off) lo = mid; else hi = mid;
	}
	return &s->idx[lo];
}

static int
__read_fd(plog_t *l, uint64_t base)
{
	char path[512];
	if (l->rfd >= 0 && l->rfd_base == base)
		return l->rfd;
	if (l->rfd >= 0) (void)close(l->rfd);
	__seg_path(path, sizeof path, l->dir, base);
	l->rfd = open(path, O_RDONLY);
	l->rfd_base = base;
	return l->rfd;
}

static void
__put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t
__get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t
__get64(const uint8_t *p)
{
	return ((uint64_t)__get32(p) << 32) | __get32(p + 4);
}

/* Read up to len bytes at pos; returns bytes read or -1. */
static long
__pread_full(int fd, uint8_t *buf, size_t len, uint64_t pos)
{
	size_t got = 0;
	while (got < len) {
		int n = xtc_aio_pread(fd, buf + got, (uint32_t)(len - got),
		    (int64_t)(pos + got));
		if (n < 0) return -1;
		if (n == 0) break;
		got += (size_t)n;
	}
	return (long)got;
}

/* Delete the oldest closed segments while the log exceeds retention. */
static void
__retain(plog_t *l)
{
	char path[512];
	while (l->retention > 0 && l->n_segs > 1 && l->total > l->retention) {
		struct seg *s = &l->segs[0];
		if (l->rfd >= 0 && l->rfd_base == s->base) {
			(void)close(l->rfd);
			l->rfd = -1;
		}
		__seg_path(path, sizeof path, l->dir, s->base);
		if (unlink(path) != 0 && errno != ENOENT)
			break;
		l->total -= s->size;
		free(s->idx);
		memmove(l->segs, l->segs + 1, (l->n_segs - 1) * sizeof *l->segs);
		l->n_segs--;
		__evict_below(l, l->segs[0].base);
	}
}

/* Open a fresh active segment starting at `base`. */
static int
__seg_create(plog_t *l, uint64_t base)
{
	char path[512];
	__seg_path(path, sizeof path, l->dir, base);
	l->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (l->fd < 0) return -1;
	l->wpos = 0;
	l->wbuf_len = 0;
	if (__seg_push(l, base) == NULL) return -1;
	return __dir_sync(l);
}

/* Write the buffered records at the end of the active segment. */
static int
__flush_wbuf(plog_t *l)
{
	size_t done = 0;
	while (done < l->wbuf_len) {
		int n = xtc_aio_pwrite(l->fd, l->wbuf + done,
		    (uint32_t)(l->wbuf_len - done), (int64_t)(l->wpos + done));
		if (n <= 0) return -1;
		done += (size_t)n;
	}
	l->wpos += done;
	l->wbuf_len = 0;
	return 0;
}

/* Close the full active segment durably and start the next one. */
static int
__roll(plog_t *l)
{
	struct seg *a = &l->segs[l->n_segs - 1];
	uint64_t next = a->next;
	uint64_t prev_base = a->base;
	if (__flush_wbuf(l) != 0 || __data_sync(l->fd) != 0) return -1;
	(void)close(l->fd);
	l->fd = -1;
	l->synced = next;
	if (__seg_create(l, next) != 0) return -1;
	__evict_below(l, prev_base);   /* keep ~2 segments in RAM */
	__retain(l);
	return 0;
}

static int
__seg_write(plog_t *l, uint64_t off, const kaka_record_t *rec)
{
	struct seg *a = &l->segs[l->n_segs - 1];
	size_t body = (size_t)rec->key_len + rec->value_len;
	size_t total = REC_HDR + body;
	uint8_t *p;

	if (a->size >= l->roll && a->next > a->base) {
		if (__roll(l) != 0) return -1;
		a = &l->segs[l->n_segs - 1];
	}
	if (l->wbuf_len + total > l->wbuf_cap &&
	    __grow(&l->wbuf, &l->wbuf_cap, l->wbuf_len + total +
	    (l->wbuf_len + total) / 2) != 0)
		return -1;
	p = l->wbuf + l->wbuf_len;
	__put32(p + 4, (uint32_t)(off >> 32));
	__put32(p + 8, (uint32_t)off);
	__put32(p + 12, rec->key_len);
	__put32(p + 16, rec->value_len);
	if (rec->key_len) memcpy(p + REC_HDR, rec->key, rec->key_len);
	if (rec->value_len)
		memcpy(p + REC_HDR + rec->key_len, rec->value, rec->value_len);
	__put32(p, crc32(p + 4, total - 4, 0));

	if (__idx_add(a, off, a->size) != 0) return -1;
	l->wbuf_len += total;
	a->size += total;
	a->next = off + 1;
	l->total += total;
	if (l->wbuf_len >= WBUF_FLUSH)
		return __flush_wbuf(l);
	return 0;
}

/* Buffer [pos, pos + max(min, want)) of the segment (clipped at limit)
 * into l->scratch.  Returns bytes buffered, or -1 if fewer than `min`. */
static long
__fill(plog_t *l, int fd, uint64_t pos, size_t min, size_t want,
    uint64_t limit)
{
	size_t len = want > min ? want : min;
	long got;
	if (pos >= limit) return -1;
	if (len > limit - pos) len = (size_t)(limit - pos);
	if (len < min) return -1;
	if (__grow(&l->scratch, &l->scratch_cap, len) != 0) return -1;
	got = __pread_full(fd, l->scratch, len, pos);
	return (got < 0 || (size_t)got < min) ? -1 : got;
}

/*
 * Scan segment s (file size fsize) from 0, building its index.  With
 * `last` (the active segment) the CRC of every record is checked and a
 * torn or corrupt tail is truncated; an older segment was synced
 * before the next one was created, so any structural defect there is
 * corruption and fails the open.
 */
static int
__seg_scan(plog_t *l, struct seg *s, int fd, uint64_t fsize, int last)
{
	uint64_t pos = 0, cur = s->base, buf_pos = 0;
	long have = 0;

	while (pos < fsize) {
		const uint8_t *r;
		uint32_t kl, vl;
		uint64_t rlen;
		if (fsize - pos < REC_HDR) goto torn;
		if (pos < buf_pos || pos + REC_HDR > buf_pos + (uint64_t)have) {
			if ((have = __fill(l, fd, pos, REC_HDR, READ_CHUNK,
			    fsize)) < 0)
				return -1;
			buf_pos = pos;
		}
		r = l->scratch + (pos - buf_pos);
		kl = __get32(r + 12);
		vl = __get32(r + 16);
		if (__get64(r + 4) != cur || kl > REC_MAX || vl > REC_MAX)
			goto torn;
		rlen = REC_HDR + (uint64_t)kl + vl;
		if (fsize - pos < rlen) goto torn;
		if (last) {
			if (pos + rlen > buf_pos + (uint64_t)have) {
				if ((have = __fill(l, fd, pos, (size_t)rlen,
				    READ_CHUNK, fsize)) < 0)
					return -1;
				buf_pos = pos;
				r = l->scratch;
			}
			if (crc32(r + 4, (size_t)rlen - 4, 0) != __get32(r))
				goto torn;
		}
		if (__idx_add(s, cur, pos) != 0) return -1;
		pos += rlen;
		cur++;
	}
	s->size = pos;
	s->next = cur;
	return 0;
torn:
	if (!last) return -1;
	s->size = pos;
	s->next = cur;
	return ftruncate(fd, (off_t)pos) == 0 ? 0 : -1;
}

static int
__name_base(const char *name, uint64_t *out)
{
	size_t i;
	uint64_t v = 0;
	if (strlen(name) != 24 || strcmp(name + 20, ".log") != 0)
		return -1;
	for (i = 0; i < 20; i++) {
		if (name[i] < '0' || name[i] > '9') return -1;
		v = v * 10 + (uint64_t)(name[i] - '0');
	}
	*out = v;
	return 0;
}

static int
__cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* Scan the directory's segments, rebuild indexes, open the active one. */
static int
__recover(plog_t *l)
{
	DIR *d = opendir(l->dir);
	struct dirent *de;
	uint64_t *bases = NULL, b;
	size_t n = 0, capn = 0, i;
	int rc = 0;

	if (d == NULL) return -1;
	while ((de = readdir(d)) != NULL) {
		if (__name_base(de->d_name, &b) != 0)
			continue;
		if (n == capn) {
			size_t nc = capn ? capn * 2 : 16;
			uint64_t *nb = realloc(bases, nc * sizeof *nb);
			if (nb == NULL) { rc = -1; break; }
			bases = nb;
			capn = nc;
		}
		bases[n++] = b;
	}
	(void)closedir(d);
	if (rc != 0) { free(bases); return -1; }
	if (n > 1)
		qsort(bases, n, sizeof *bases, __cmp_u64);

	for (i = 0; i < n && rc == 0; i++) {
		char path[512];
		struct seg *s;
		int fd, last = (i == n - 1);
		off_t end;
		if (i > 0 && bases[i] != l->segs[l->n_segs - 1].next) {
			rc = -1;                 /* gap or overlap: corrupt */
			break;
		}
		if ((s = __seg_push(l, bases[i])) == NULL) { rc = -1; break; }
		__seg_path(path, sizeof path, l->dir, bases[i]);
		fd = open(path, last ? O_RDWR : O_RDONLY);
		if (fd < 0) { rc = -1; break; }
		end = lseek(fd, 0, SEEK_END);
		if (end < 0 || __seg_scan(l, s, fd, (uint64_t)end, last) != 0) {
			(void)close(fd);
			rc = -1;
			break;
		}
		l->total += s->size;
		if (last) {
			l->fd = fd;
			l->wpos = s->size;
		} else {
			(void)close(fd);
		}
	}
	free(bases);
	if (rc != 0) return -1;
	if (n == 0)
		return __seg_create(l, 0);
	l->hw = l->synced = l->segs[l->n_segs - 1].next;
	l->base = l->hw;                 /* RAM window starts empty */
	return 0;
}

/* ---- public API ---- */

static int
__plog_alloc(plog_t **out)
{
	plog_t *l = calloc(1, sizeof *l);
	if (l == NULL) return -1;
	l->cap = 1024;
	l->slots = calloc((size_t)l->cap, sizeof *l->slots);
	if (l->slots == NULL) { free(l); return -1; }
	l->fd = -1;
	l->rfd = -1;
	l->roll = SEG_DEFAULT_ROLL;
	*out = l;
	return 0;
}

int
plog_create(plog_t **out)
{
	if (out == NULL) return -1;
	return __plog_alloc(out);
}

int
plog_create_ex(const char *dir, size_t seg_roll_bytes, plog_t **out)
{
	plog_t *l;
	if (out == NULL) return -1;
	if (dir == NULL) return __plog_alloc(out);
	if (__plog_alloc(&l) != 0) return -1;
	l->dir = strdup(dir);
	if (l->dir == NULL) { plog_destroy(l); return -1; }
	if (seg_roll_bytes > 0) l->roll = seg_roll_bytes;
	if (__recover(l) != 0) { plog_destroy(l); return -1; }
	*out = l;
	return 0;
}

void
plog_set_retention(plog_t *l, uint64_t bytes)
{
	if (l != NULL)
		l->retention = bytes;
}

void
plog_destroy(plog_t *l)
{
	uint64_t i;
	if (l == NULL) return;
	if (l->fd >= 0) {
		(void)plog_sync(l);
		(void)close(l->fd);
	}
	if (l->rfd >= 0) (void)close(l->rfd);
	for (i = 0; i < l->count; i++)
		free(l->slots[i].bytes);
	for (i = 0; i < l->n_segs; i++)
		free(l->segs[i].idx);
	free(l->segs);
	free(l->slots);
	free(l->scratch);
	free(l->cold);
	free(l->wbuf);
	free(l->dir);
	free(l);
}

int64_t
plog_append(plog_t *l, const kaka_record_t *rec)
{
	uint64_t off;
	if (l == NULL || rec == NULL || l->failed) return -1;
	if (rec->key_len > REC_MAX || rec->value_len > REC_MAX) return -1;
	off = l->hw;
	if (l->dir != NULL) {
		if (__seg_write(l, off, rec) != 0) {
			l->failed = 1;           /* fail-stop: state unknown */
			return -1;
		}
		l->hw = off + 1;
		/* The RAM copy is a cache here: without memory, serve the
		 * record from disk instead. */
		if (__mem_append(l, rec) != 0)
			plog_evict_all(l);
		return (int64_t)off;
	}
	if (__mem_append(l, rec) != 0) return -1;
	l->hw = l->synced = off + 1;
	return (int64_t)off;
}

int
plog_sync(plog_t *l)
{
	if (l == NULL) return -1;
	if (l->failed) return -1;
	if (l->dir == NULL || l->synced == l->hw) return 0;
	if (__flush_wbuf(l) != 0 || __data_sync(l->fd) != 0) {
		l->failed = 1;
		return -1;
	}
	l->synced = l->hw;
	return 0;
}

int
plog_failed(const plog_t *l)
{
	return l == NULL || l->failed;
}

uint64_t
plog_high_water(const plog_t *l)
{
	return l == NULL ? 0 : l->hw;
}

uint64_t
plog_synced(const plog_t *l)
{
	return l == NULL ? 0 : l->synced;
}

uint64_t
plog_start(const plog_t *l)
{
	if (l == NULL) return 0;
	if (l->dir == NULL) return l->base;
	return l->n_segs > 0 ? l->segs[0].base : l->hw;
}

/* Copy records [*offp, stop) -- all below the RAM window -- from the
 * segment files into dst + used.  Advances *offp; returns the new
 * `used`, or -1 on an I/O error or a CRC mismatch. */
static long
__fetch_cold(plog_t *l, uint64_t *offp, uint64_t stop, uint8_t *dst,
    size_t cap, size_t used, uint32_t *n, size_t *need)
{
	uint64_t off = *offp;

	while (off < stop) {
		const struct seg *s = __seg_find(l, off);
		const struct idx_ent *e;
		uint64_t pos, cur, seg_stop, buf_pos = 0;
		long have = 0;
		int fd;
		if (s == NULL || s->n_idx == 0) return -1;
		e = __idx_find(s, off);
		pos = e->pos;
		cur = e->off;
		buf_pos = pos + 1;           /* nothing buffered yet */
		seg_stop = stop < s->next ? stop : s->next;
		if ((fd = __read_fd(l, s->base)) < 0) return -1;
		while (off < seg_stop) {
			const uint8_t *r;
			uint32_t kl, vl;
			size_t rlen, wire;
			size_t want = IDX_INTERVAL + REC_HDR + (cap - used);
			if (want > READ_CHUNK) want = READ_CHUNK;
			if (pos < buf_pos ||
			    pos + REC_HDR > buf_pos + (uint64_t)have) {
				if ((have = __fill(l, fd, pos, REC_HDR, want,
				    s->size)) < 0)
					return -1;
				buf_pos = pos;
			}
			r = l->scratch + (pos - buf_pos);
			kl = __get32(r + 12);
			vl = __get32(r + 16);
			if (__get64(r + 4) != cur || kl > REC_MAX || vl > REC_MAX)
				return -1;
			rlen = REC_HDR + (size_t)kl + vl;
			if (pos + rlen > s->size) return -1;
			if (cur >= off) {
				wire = rlen - 4;
				if (cap - used < wire) {
					if (*n == 0 && need != NULL)
						*need = wire;
					*offp = off;
					return (long)used;
				}
				if (pos + rlen > buf_pos + (uint64_t)have) {
					if ((have = __fill(l, fd, pos, rlen, want,
					    s->size)) < 0)
						return -1;
					buf_pos = pos;
					r = l->scratch;
				}
				if (crc32(r + 4, wire, 0) != __get32(r))
					return -1;
				memcpy(dst + used, r + 4, wire);
				used += wire;
				(*n)++;
				off = cur + 1;
			}
			pos += rlen;
			cur++;
		}
	}
	*offp = off;
	return (long)used;
}

long
plog_fetch(plog_t *l, uint64_t *offset, uint64_t end, uint8_t *dst,
    size_t cap, uint32_t *n, size_t *need)
{
	size_t used = 0;
	uint64_t off, cold_end;

	if (l == NULL || offset == NULL || dst == NULL || n == NULL) return -1;
	*n = 0;
	off = *offset;
	if (end > l->hw) end = l->hw;
	if (off < plog_start(l)) off = plog_start(l);
	cold_end = end < l->base ? end : l->base;
	if (off < cold_end) {
		long r = __fetch_cold(l, &off, cold_end, dst, cap, used, n, need);
		if (r < 0) return -1;
		used = (size_t)r;
		if (off < cold_end) {             /* dst full */
			*offset = off;
			return (long)used;
		}
	}
	while (off < end && off - l->base < l->count) {
		const struct slot *s = &l->slots[off - l->base];
		size_t kv = (size_t)s->key_len + s->value_len;
		uint8_t *p = dst + used;
		if (cap - used < WIRE_HDR + kv) {
			if (*n == 0 && need != NULL) *need = WIRE_HDR + kv;
			break;
		}
		__put32(p, (uint32_t)(off >> 32));
		__put32(p + 4, (uint32_t)off);
		__put32(p + 8, s->key_len);
		__put32(p + 12, s->value_len);
		if (kv > 0) memcpy(p + WIRE_HDR, s->bytes, kv);
		used += WIRE_HDR + kv;
		(*n)++;
		off++;
	}
	*offset = off;
	return (long)used;
}

int
plog_read(plog_t *l, uint64_t offset, kaka_record_t *rec)
{
	struct slot *s;
	uint32_t n = 0;
	size_t need = 0;
	uint64_t at;
	long w;
	if (l == NULL || rec == NULL) return -1;
	if (offset >= l->hw || offset < plog_start(l)) return 0;
	if (offset >= l->base) {
		s = &l->slots[offset - l->base];
		rec->key = s->bytes;
		rec->key_len = s->key_len;
		rec->value = (s->bytes != NULL) ? s->bytes + s->key_len : NULL;
		rec->value_len = s->value_len;
		return 1;
	}
	if (l->cold_cap < WIRE_HDR && __grow(&l->cold, &l->cold_cap, 256) != 0)
		return -1;
	at = offset;
	w = plog_fetch(l, &at, offset + 1, l->cold, l->cold_cap, &n, &need);
	if (w >= 0 && n == 0 && need > 0) {
		if (__grow(&l->cold, &l->cold_cap, need) != 0) return -1;
		at = offset;
		w = plog_fetch(l, &at, offset + 1, l->cold, l->cold_cap,
		    &n, &need);
	}
	if (w < 0) return -1;
	if (n != 1 || __get64(l->cold) != offset) return 0;
	rec->key_len = __get32(l->cold + 8);
	rec->value_len = __get32(l->cold + 12);
	rec->key = l->cold + WIRE_HDR;
	rec->value = l->cold + WIRE_HDR + rec->key_len;
	return 1;
}

uint64_t
plog_count(const plog_t *l)
{
	return l == NULL ? 0 : l->hw;
}

uint64_t
plog_mem_records(const plog_t *l)
{
	return l == NULL ? 0 : l->count;
}

uint64_t
plog_mem_bytes(const plog_t *l)
{
	return l == NULL ? 0 : l->mem_bytes;
}

uint64_t
plog_segments(const plog_t *l)
{
	return l == NULL ? 0 : (uint64_t)l->n_segs;
}
