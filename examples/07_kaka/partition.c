/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/partition.c -- partition log core.
 *
 *	Phase 1 storage is a growable vector of offset slots, each
 *	holding a malloc'd copy of one record's key+value bytes.  Append
 *	is amortized O(1); read is O(1) by offset minus base.  Because
 *	the owning xtc_proc is the only writer, none of this needs a
 *	lock -- the proc mailbox is the serialization point.
 *
 *	Phase 2 adds optional durability: when a log is created with a
 *	directory (plog_create_ex), every append is also written to a
 *	CRC-framed segment file.  Segments roll to a new file once they
 *	exceed a byte threshold; segment files are named by their base
 *	offset, Kafka-style.  On open the segments are scanned and
 *	replayed into the in-memory vector, so reads stay in memory and
 *	fast while the on-disk log survives a restart.  A trailing
 *	partial or CRC-failing record truncates the segment at the
 *	corruption boundary (same discipline as the rexis Bitcask).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "partition.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SEG_DEFAULT_ROLL   (1u * 1024u * 1024u)   /* 1 MiB */
#define REC_MAX            (16u * 1024u * 1024u)
/* On-disk record: crc32 | offset u64 | key_len u32 | val_len u32 | k | v */
#define REC_HDR            (4 + 8 + 4 + 4)

struct slot {
	uint8_t *bytes;       /* key||value, malloc'd */
	uint32_t key_len;
	uint32_t value_len;
};

struct plog {
	struct slot *slots;
	uint64_t     base;     /* offset of slots[0]; 0 until compaction */
	uint64_t     count;    /* live records */
	uint64_t     cap;      /* slots allocated */

	/* Phase 2 persistence (NULL/-1 when in-memory only). */
	char        *dir;
	int          seg_fd;       /* current (open) segment, or -1 */
	uint64_t     seg_base;     /* base offset of the current segment */
	uint64_t     seg_bytes;    /* bytes written to the current segment */
	size_t       seg_roll;     /* roll threshold */
	uint64_t     active_base;  /* base of the newest segment found in
	                            * recovery; the one to keep appending to */
};

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

/* ---- in-memory vector (used by both modes) ---- */

static int64_t
__mem_append(plog_t *l, const kaka_record_t *rec)
{
	struct slot *s;
	size_t total;

	if (l->count == l->cap) {
		uint64_t ncap = l->cap * 2;
		struct slot *ns;
		if (ncap < l->cap) return -1;
		ns = realloc(l->slots, (size_t)ncap * sizeof *ns);
		if (ns == NULL) return -1;
		l->slots = ns;
		l->cap = ncap;
	}
	total = (size_t)rec->key_len + rec->value_len;
	s = &l->slots[l->count];
	s->bytes = (total > 0) ? malloc(total) : NULL;
	if (total > 0 && s->bytes == NULL) return -1;
	if (rec->key_len)
		memcpy(s->bytes, rec->key, rec->key_len);
	if (rec->value_len)
		memcpy(s->bytes + rec->key_len, rec->value, rec->value_len);
	s->key_len = rec->key_len;
	s->value_len = rec->value_len;
	return (int64_t)(l->base + l->count++);
}

/* ---- segment file helpers ---- */

static void
__seg_path(char *buf, size_t cap, const char *dir, uint64_t base)
{
	snprintf(buf, cap, "%s/%020llu.log", dir, (unsigned long long)base);
}

/* Open (creating) the segment whose base offset is `base` and seek to
 * its end.  Sets seg_fd / seg_base / seg_bytes. */
static int
__seg_open(plog_t *l, uint64_t base)
{
	char path[512];
	off_t end;
	if (l->seg_fd >= 0) { (void)close(l->seg_fd); l->seg_fd = -1; }
	__seg_path(path, sizeof path, l->dir, base);
	l->seg_fd = open(path, O_RDWR | O_CREAT, 0644);
	if (l->seg_fd < 0) return -1;
	end = lseek(l->seg_fd, 0, SEEK_END);
	if (end < 0) return -1;
	l->seg_base  = base;
	l->seg_bytes = (uint64_t)end;
	return 0;
}

/* Frame and write one record at the current segment end, rolling to a
 * fresh segment first if the threshold is exceeded. */
static int
__seg_write(plog_t *l, uint64_t offset, const kaka_record_t *rec)
{
	uint8_t hdr[REC_HDR];
	uint8_t crchdr[REC_HDR - 4];
	uint32_t crc;
	size_t body = (size_t)rec->key_len + rec->value_len;
	ssize_t n;

	if (l->seg_bytes > 0 && l->seg_bytes >= l->seg_roll) {
		if (__seg_open(l, offset) != 0) return -1;   /* roll */
	}

	/* crchdr = offset | key_len | val_len */
	crchdr[0] = (uint8_t)(offset >> 56); crchdr[1] = (uint8_t)(offset >> 48);
	crchdr[2] = (uint8_t)(offset >> 40); crchdr[3] = (uint8_t)(offset >> 32);
	crchdr[4] = (uint8_t)(offset >> 24); crchdr[5] = (uint8_t)(offset >> 16);
	crchdr[6] = (uint8_t)(offset >> 8);  crchdr[7] = (uint8_t)offset;
	crchdr[8]  = (uint8_t)(rec->key_len >> 24); crchdr[9]  = (uint8_t)(rec->key_len >> 16);
	crchdr[10] = (uint8_t)(rec->key_len >> 8);  crchdr[11] = (uint8_t)rec->key_len;
	crchdr[12] = (uint8_t)(rec->value_len >> 24); crchdr[13] = (uint8_t)(rec->value_len >> 16);
	crchdr[14] = (uint8_t)(rec->value_len >> 8);  crchdr[15] = (uint8_t)rec->value_len;

	crc = crc32(crchdr, sizeof crchdr, 0);
	if (rec->key_len)   crc = crc32(rec->key, rec->key_len, crc);
	if (rec->value_len) crc = crc32(rec->value, rec->value_len, crc);

	hdr[0] = (uint8_t)(crc >> 24); hdr[1] = (uint8_t)(crc >> 16);
	hdr[2] = (uint8_t)(crc >> 8);  hdr[3] = (uint8_t)crc;
	memcpy(hdr + 4, crchdr, sizeof crchdr);

	n = write(l->seg_fd, hdr, REC_HDR);
	if (n != (ssize_t)REC_HDR) return -1;
	if (rec->key_len) {
		n = write(l->seg_fd, rec->key, rec->key_len);
		if (n != (ssize_t)rec->key_len) return -1;
	}
	if (rec->value_len) {
		n = write(l->seg_fd, rec->value, rec->value_len);
		if (n != (ssize_t)rec->value_len) return -1;
	}
	l->seg_bytes += REC_HDR + body;
	return 0;
}

/* Scan one segment file, replaying valid records into the in-memory
 * vector.  Truncates at the first partial/corrupt record. */
static int
__seg_replay(plog_t *l, const char *path)
{
	int fd = open(path, O_RDWR);
	off_t fsize, off = 0;
	uint8_t hdr[REC_HDR];
	uint8_t *body = NULL;
	size_t body_cap = 0;
	if (fd < 0) return -1;
	fsize = lseek(fd, 0, SEEK_END);
	if (fsize < 0) { (void)close(fd); return -1; }

	while ((uint64_t)fsize - (uint64_t)off >= REC_HDR) {
		uint32_t crc_stored, kl, vl, crc_calc;
		uint64_t recoff;
		size_t blen;
		kaka_record_t r;
		ssize_t got = pread(fd, hdr, REC_HDR, off);
		if (got != (ssize_t)REC_HDR) break;
		crc_stored = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
		             ((uint32_t)hdr[2] << 8)  | hdr[3];
		recoff = ((uint64_t)hdr[4] << 56) | ((uint64_t)hdr[5] << 48) |
		         ((uint64_t)hdr[6] << 40) | ((uint64_t)hdr[7] << 32) |
		         ((uint64_t)hdr[8] << 24) | ((uint64_t)hdr[9] << 16) |
		         ((uint64_t)hdr[10] << 8) | hdr[11];
		kl = ((uint32_t)hdr[12] << 24) | ((uint32_t)hdr[13] << 16) |
		     ((uint32_t)hdr[14] << 8) | hdr[15];
		vl = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) |
		     ((uint32_t)hdr[18] << 8) | hdr[19];
		if (kl > REC_MAX || vl > REC_MAX) { if (ftruncate(fd, off) != 0) { /* best effort */ } break; }
		blen = (size_t)kl + vl;
		if ((uint64_t)fsize - (uint64_t)off - REC_HDR < blen) {
			if (ftruncate(fd, off) != 0) { /* best effort */ } break;
		}
		if (blen > body_cap) {
			uint8_t *nb = realloc(body, blen ? blen : 1);
			if (nb == NULL) break;
			body = nb; body_cap = blen;
		}
		if (blen > 0 && pread(fd, body, blen, off + REC_HDR)
		    != (ssize_t)blen) { if (ftruncate(fd, off) != 0) { /* best effort */ } break; }
		crc_calc = crc32(hdr + 4, REC_HDR - 4, 0);
		crc_calc = crc32(body, blen, crc_calc);
		if (crc_calc != crc_stored) { if (ftruncate(fd, off) != 0) { /* best effort */ } break; }
		(void)recoff;   /* offsets are contiguous; the vector tracks them */
		r.key = body; r.key_len = kl;
		r.value = body + kl; r.value_len = vl;
		(void)__mem_append(l, &r);
		off += REC_HDR + blen;
	}
	free(body);
	(void)close(fd);
	return 0;
}

/* Recover all segments in the directory, in base-offset order. */
static int
__recover(plog_t *l)
{
	DIR *d = opendir(l->dir);
	struct dirent *de;
	/* Collect .log basenames, sort, replay. */
	char (*names)[32] = NULL;
	size_t n = 0, capn = 0;
	if (d == NULL) return 0;     /* empty/new dir */
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcmp(de->d_name + len - 4, ".log") != 0)
			continue;
		if (len >= sizeof names[0]) continue;
		if (n == capn) {
			size_t nc = capn ? capn * 2 : 8;
			void *nn = realloc(names, nc * sizeof names[0]);
			if (nn == NULL) break;
			names = nn; capn = nc;
		}
		memcpy(names[n++], de->d_name, len + 1);
	}
	(void)closedir(d);
	/* Insertion sort by name (zero-padded -> lexical == numeric). */
	{
		size_t i, j;
		for (i = 1; i < n; i++) {
			char tmp[32];
			memcpy(tmp, names[i], sizeof tmp);
			for (j = i; j > 0 && strcmp(names[j - 1], tmp) > 0; j--)
				memcpy(names[j], names[j - 1], sizeof tmp);
			memcpy(names[j], tmp, sizeof tmp);
		}
	}
	{
		size_t i;
		char path[512];
		for (i = 0; i < n; i++) {
			snprintf(path, sizeof path, "%s/%s", l->dir, names[i]);
			(void)__seg_replay(l, path);
		}
		/* The newest segment (last after sort) is where appends
		 * continue.  Parse its base offset from the zero-padded
		 * filename. */
		if (n > 0)
			l->active_base = strtoull(names[n - 1], NULL, 10);
	}
	free(names);
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
	l->seg_fd = -1;
	l->seg_roll = SEG_DEFAULT_ROLL;
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
	if (seg_roll_bytes > 0) l->seg_roll = seg_roll_bytes;

	/* Replay any existing segments into the in-memory vector. */
	if (__recover(l) != 0) { plog_destroy(l); return -1; }

	/* Open (or create) the active segment: continue the newest one
	 * recovery found, or start segment 0 on a fresh directory. */
	if (__seg_open(l, l->count == 0 ? 0 : l->active_base) != 0) {
		plog_destroy(l); return -1;
	}
	*out = l;
	return 0;
}

void
plog_destroy(plog_t *l)
{
	uint64_t i;
	if (l == NULL) return;
	if (l->seg_fd >= 0) { (void)fsync(l->seg_fd); (void)close(l->seg_fd); }
	for (i = 0; i < l->count; i++)
		free(l->slots[i].bytes);
	free(l->slots);
	free(l->dir);
	free(l);
}

int64_t
plog_append(plog_t *l, const kaka_record_t *rec)
{
	int64_t off;
	if (l == NULL || rec == NULL) return -1;
	if (rec->key_len > REC_MAX || rec->value_len > REC_MAX) return -1;
	off = (int64_t)(l->base + l->count);
	/* Persist first, so a successful return means it is durable. */
	if (l->dir != NULL) {
		if (__seg_write(l, (uint64_t)off, rec) != 0) return -1;
	}
	return __mem_append(l, rec);
}

uint64_t
plog_high_water(const plog_t *l)
{
	return l == NULL ? 0 : l->base + l->count;
}

int
plog_read(plog_t *l, uint64_t offset, kaka_record_t *rec)
{
	uint64_t idx;
	struct slot *s;
	if (l == NULL || rec == NULL) return -1;
	if (offset < l->base) return -1;
	idx = offset - l->base;
	if (idx >= l->count) return 0;
	s = &l->slots[idx];
	rec->key = s->bytes;
	rec->key_len = s->key_len;
	rec->value = (s->bytes != NULL) ? s->bytes + s->key_len : NULL;
	rec->value_len = s->value_len;
	return 1;
}

uint64_t
plog_count(const plog_t *l)
{
	return l == NULL ? 0 : l->count;
}
