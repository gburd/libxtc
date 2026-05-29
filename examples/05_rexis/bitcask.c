/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/05_rexis/bitcask.c -- Bitcask-style on-disk KV.
 *
 * On-disk record format (network byte order; CRC32 over body):
 *
 *   uint32_t  crc;        // CRC32 of (timestamp .. value)
 *   uint64_t  timestamp;  // monotonic ns of write
 *   uint32_t  key_len;
 *   uint32_t  val_len;    // 0xFFFFFFFFu = tombstone
 *   uint8_t   key[key_len];
 *   uint8_t   val[val_len];   // omitted if tombstone
 *
 * Recovery on open: scan the file from start, rebuild the in-memory
 * hash index by replaying every record (the latest entry per key
 * wins; tombstones erase).
 *
 * v1 limitations:
 *   * single data file; no rotation, no merge.
 *   * one writer at a time (no concurrent put).  Multiple readers
 *     are fine.
 *   * value size capped at 16 MiB to limit recovery cost.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "bitcask.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_VAL_LEN  (16u * 1024u * 1024u)

/* ---- CRC32 (IEEE 802.3) -------------------------------------- */

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

/* ---- bytewise NBO helpers ------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v) {
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_u64(uint8_t *p, uint64_t v) {
	put_u32(p, (uint32_t)(v >> 32));
	put_u32(p + 4, (uint32_t)v);
}
static uint32_t get_u32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint64_t get_u64(const uint8_t *p) {
	return ((uint64_t)get_u32(p) << 32) | (uint64_t)get_u32(p + 4);
}

#define HDR_SIZE  (4 /*crc*/ + 8 /*ts*/ + 4 /*kl*/ + 4 /*vl*/)
#define TOMBSTONE 0xffffffffu

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- in-memory index ----------------------------------------- */

#define IDX_BUCKETS 4096

struct idx_entry {
	uint8_t            *key;       /* malloc'd copy */
	size_t              key_len;
	uint64_t            offset;    /* file offset of value */
	uint32_t            val_len;   /* on-disk length */
	uint64_t            timestamp;
	struct idx_entry   *next;
};

static uint32_t
__hash_key(const void *key, size_t len)
{
	const uint8_t *p = key;
	uint32_t h = 2166136261u;
	size_t i;
	for (i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
	return h ? h : 1;
}

struct bitcask {
	pthread_mutex_t      lock;
	int                  fd;
	uint64_t             write_offset;
	struct idx_entry    *buckets[IDX_BUCKETS];
	uint64_t             n_keys;
	uint64_t             bytes_used;
	uint64_t             bytes_dead;
	uint64_t             puts, gets, dels, hits, misses;
};

static struct idx_entry *
__idx_lookup(struct bitcask *bc, const void *key, size_t key_len)
{
	uint32_t h = __hash_key(key, key_len) % IDX_BUCKETS;
	struct idx_entry *e = bc->buckets[h];
	while (e != NULL) {
		if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
			return e;
		e = e->next;
	}
	return NULL;
}

/* Insert/replace.  Updates bytes_dead with the prior entry's size
 * if any. */
static int
__idx_put(struct bitcask *bc, const void *key, size_t key_len,
          uint64_t offset, uint32_t val_len, uint64_t ts)
{
	uint32_t h = __hash_key(key, key_len) % IDX_BUCKETS;
	struct idx_entry *e, **link;
	for (link = &bc->buckets[h], e = *link; e != NULL;
	     link = &e->next, e = *link) {
		if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
			bc->bytes_dead += HDR_SIZE + e->key_len + e->val_len;
			e->offset = offset;
			e->val_len = val_len;
			e->timestamp = ts;
			return 0;
		}
	}
	e = calloc(1, sizeof *e);
	if (e == NULL) return -1;
	e->key = malloc(key_len);
	if (e->key == NULL) { free(e); return -1; }
	memcpy(e->key, key, key_len);
	e->key_len = key_len;
	e->offset = offset;
	e->val_len = val_len;
	e->timestamp = ts;
	e->next = bc->buckets[h];
	bc->buckets[h] = e;
	bc->n_keys++;
	return 0;
}

static int
__idx_del(struct bitcask *bc, const void *key, size_t key_len)
{
	uint32_t h = __hash_key(key, key_len) % IDX_BUCKETS;
	struct idx_entry *e, **link;
	for (link = &bc->buckets[h]; (e = *link) != NULL; link = &e->next) {
		if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
			*link = e->next;
			bc->bytes_dead += HDR_SIZE + e->key_len + e->val_len;
			free(e->key); free(e);
			bc->n_keys--;
			return 0;
		}
	}
	return 1;
}

static void
__idx_free_all(struct bitcask *bc)
{
	int i;
	for (i = 0; i < IDX_BUCKETS; i++) {
		struct idx_entry *e = bc->buckets[i], *n;
		while (e != NULL) {
			n = e->next;
			free(e->key); free(e);
			e = n;
		}
		bc->buckets[i] = NULL;
	}
}

/* ---- recovery ----------------------------------------------- */

static int
__recover(struct bitcask *bc)
{
	off_t fsize = lseek(bc->fd, 0, SEEK_END);
	uint64_t off = 0;
	uint8_t  hdr[HDR_SIZE];
	uint8_t *body = NULL;
	size_t   body_cap = 0;
	int      rc = 0;

	if (fsize < 0) return -1;

	while ((uint64_t)fsize - off >= HDR_SIZE) {
		ssize_t n = pread(bc->fd, hdr, HDR_SIZE, (off_t)off);
		if (n != (ssize_t)HDR_SIZE) {
			/* Trailing garbage; truncate. */
			(void)ftruncate(bc->fd, (off_t)off);
			break;
		}
		uint32_t crc_stored = get_u32(hdr);
		uint64_t ts         = get_u64(hdr + 4);
		uint32_t kl         = get_u32(hdr + 12);
		uint32_t vl         = get_u32(hdr + 16);
		uint32_t body_len = kl + (vl == TOMBSTONE ? 0 : vl);
		if (kl > MAX_VAL_LEN || (vl != TOMBSTONE && vl > MAX_VAL_LEN)) {
			(void)ftruncate(bc->fd, (off_t)off);
			break;
		}
		if ((uint64_t)fsize - off - HDR_SIZE < body_len) {
			(void)ftruncate(bc->fd, (off_t)off);
			break;
		}
		if (body_len > body_cap) {
			void *nb = realloc(body, body_len);
			if (nb == NULL) { rc = -1; break; }
			body = nb;
			body_cap = body_len;
		}
		if (body_len > 0) {
			n = pread(bc->fd, body, body_len,
			    (off_t)(off + HDR_SIZE));
			if (n != (ssize_t)body_len) {
				(void)ftruncate(bc->fd, (off_t)off);
				break;
			}
		}
		/* Verify CRC. */
		{
			uint32_t crc_calc;
			uint8_t hdr_for_crc[HDR_SIZE - 4];
			memcpy(hdr_for_crc, hdr + 4, HDR_SIZE - 4);
			crc_calc = crc32(hdr_for_crc, HDR_SIZE - 4, 0);
			crc_calc = crc32(body, body_len, crc_calc);
			if (crc_calc != crc_stored) {
				/* Corrupt record at this offset; truncate
				 * and stop -- subsequent records may also
				 * be corrupt. */
				(void)ftruncate(bc->fd, (off_t)off);
				break;
			}
		}
		if (vl == TOMBSTONE)
			(void)__idx_del(bc, body, kl);
		else
			(void)__idx_put(bc, body, kl,
			    off + HDR_SIZE + kl, vl, ts);
		off += HDR_SIZE + body_len;
	}
	bc->write_offset = off;
	free(body);
	return rc;
}

/* ---- public API --------------------------------------------- */

int
bitcask_open(const char *dir, bitcask_t **out)
{
	bitcask_t *bc;
	char path[512];
	if (dir == NULL || out == NULL) return -1;
	bc = calloc(1, sizeof *bc);
	if (bc == NULL) return -1;
	pthread_mutex_init(&bc->lock, NULL);
	snprintf(path, sizeof path, "%s/bitcask.data", dir);
	bc->fd = open(path, O_RDWR | O_CREAT, 0644);
	if (bc->fd < 0) { free(bc); return -1; }
	if (__recover(bc) != 0) {
		(void)close(bc->fd); free(bc); return -1;
	}
	*out = bc;
	return 0;
}

void
bitcask_close(bitcask_t *bc)
{
	if (bc == NULL) return;
	(void)fsync(bc->fd);
	(void)close(bc->fd);
	__idx_free_all(bc);
	pthread_mutex_destroy(&bc->lock);
	free(bc);
}

int
bitcask_put(bitcask_t *bc, const void *key, size_t key_len,
            const void *val, size_t val_len)
{
	uint8_t hdr[HDR_SIZE];
	struct iovec { const void *base; size_t len; };
	uint64_t ts = (uint64_t)now_ns();
	uint32_t crc;
	uint8_t hdr_for_crc[HDR_SIZE - 4];
	ssize_t n;
	uint64_t off;

	if (bc == NULL || key == NULL || val == NULL ||
	    key_len == 0 || key_len > MAX_VAL_LEN ||
	    val_len > MAX_VAL_LEN)
		return -1;

	put_u64(hdr_for_crc + 0, ts);
	put_u32(hdr_for_crc + 8, (uint32_t)key_len);
	put_u32(hdr_for_crc + 12, (uint32_t)val_len);
	crc = crc32(hdr_for_crc, sizeof hdr_for_crc, 0);
	crc = crc32(key, key_len, crc);
	crc = crc32(val, val_len, crc);

	put_u32(hdr, crc);
	memcpy(hdr + 4, hdr_for_crc, sizeof hdr_for_crc);

	pthread_mutex_lock(&bc->lock);
	off = bc->write_offset;
	n = pwrite(bc->fd, hdr, HDR_SIZE, (off_t)off);
	if (n != (ssize_t)HDR_SIZE) goto io_err;
	n = pwrite(bc->fd, key, key_len, (off_t)(off + HDR_SIZE));
	if (n != (ssize_t)key_len) goto io_err;
	n = pwrite(bc->fd, val, val_len,
	    (off_t)(off + HDR_SIZE + key_len));
	if (n != (ssize_t)val_len) goto io_err;
	bc->write_offset = off + HDR_SIZE + key_len + val_len;
	__idx_put(bc, key, key_len, off + HDR_SIZE + key_len,
	    (uint32_t)val_len, ts);
	bc->bytes_used += HDR_SIZE + key_len + val_len;
	bc->puts++;
	pthread_mutex_unlock(&bc->lock);
	return 0;
io_err:
	pthread_mutex_unlock(&bc->lock);
	return -1;
}

int
bitcask_get(bitcask_t *bc, const void *key, size_t key_len,
            void *val_out, size_t val_buf_size, size_t *val_len)
{
	struct idx_entry *e;
	ssize_t n;
	size_t copy_len;
	if (bc == NULL || key == NULL || val_len == NULL) return -1;
	pthread_mutex_lock(&bc->lock);
	bc->gets++;
	e = __idx_lookup(bc, key, key_len);
	if (e == NULL) {
		bc->misses++;
		pthread_mutex_unlock(&bc->lock);
		*val_len = 0;
		return 1;
	}
	bc->hits++;
	*val_len = e->val_len;
	copy_len = (e->val_len < val_buf_size) ? e->val_len : val_buf_size;
	pthread_mutex_unlock(&bc->lock);
	if (copy_len > 0 && val_out != NULL) {
		n = pread(bc->fd, val_out, copy_len, (off_t)e->offset);
		if (n != (ssize_t)copy_len) return -1;
	}
	return 0;
}

int
bitcask_size(bitcask_t *bc, const void *key, size_t key_len, size_t *val_len)
{
	struct idx_entry *e;
	if (bc == NULL || key == NULL || val_len == NULL) return -1;
	pthread_mutex_lock(&bc->lock);
	e = __idx_lookup(bc, key, key_len);
	if (e == NULL) {
		pthread_mutex_unlock(&bc->lock);
		*val_len = 0;
		return 1;
	}
	*val_len = e->val_len;
	pthread_mutex_unlock(&bc->lock);
	return 0;
}

int
bitcask_del(bitcask_t *bc, const void *key, size_t key_len)
{
	uint8_t hdr[HDR_SIZE];
	uint8_t hdr_for_crc[HDR_SIZE - 4];
	uint32_t crc;
	uint64_t ts = (uint64_t)now_ns();
	ssize_t n;
	int found;
	if (bc == NULL || key == NULL || key_len == 0) return -1;

	put_u64(hdr_for_crc + 0, ts);
	put_u32(hdr_for_crc + 8, (uint32_t)key_len);
	put_u32(hdr_for_crc + 12, TOMBSTONE);
	crc = crc32(hdr_for_crc, sizeof hdr_for_crc, 0);
	crc = crc32(key, key_len, crc);
	put_u32(hdr, crc);
	memcpy(hdr + 4, hdr_for_crc, sizeof hdr_for_crc);

	pthread_mutex_lock(&bc->lock);
	bc->dels++;
	found = (__idx_lookup(bc, key, key_len) != NULL);
	n = pwrite(bc->fd, hdr, HDR_SIZE, (off_t)bc->write_offset);
	if (n != (ssize_t)HDR_SIZE) {
		pthread_mutex_unlock(&bc->lock);
		return -1;
	}
	n = pwrite(bc->fd, key, key_len,
	    (off_t)(bc->write_offset + HDR_SIZE));
	if (n != (ssize_t)key_len) {
		pthread_mutex_unlock(&bc->lock);
		return -1;
	}
	bc->write_offset += HDR_SIZE + key_len;
	if (found) __idx_del(bc, key, key_len);
	pthread_mutex_unlock(&bc->lock);
	return found ? 0 : 1;
}

int
bitcask_iterate(bitcask_t *bc, bitcask_iter_fn fn, void *user)
{
	int i;
	size_t nkeys = 0, capacity = 0;
	struct snap { uint8_t *key; size_t key_len; };
	struct snap *snap = NULL;
	int rc = 0;

	if (bc == NULL || fn == NULL) return -1;

	/* Phase 1: snapshot all (key, key_len) pairs while holding the
	 * lock.  Releasing the lock before invoking user callbacks lets
	 * those callbacks safely re-enter bitcask_get / bitcask_size /
	 * bitcask_put without deadlocking on this mutex. */
	pthread_mutex_lock(&bc->lock);
	capacity = bc->n_keys;
	if (capacity > 0) {
		snap = calloc(capacity, sizeof *snap);
		if (snap == NULL) {
			pthread_mutex_unlock(&bc->lock);
			return -1;
		}
		for (i = 0; i < IDX_BUCKETS && nkeys < capacity; i++) {
			struct idx_entry *e;
			for (e = bc->buckets[i]; e != NULL && nkeys < capacity;
			     e = e->next) {
				snap[nkeys].key = malloc(e->key_len);
				if (snap[nkeys].key == NULL) {
					size_t j;
					for (j = 0; j < nkeys; j++)
						free(snap[j].key);
					free(snap);
					pthread_mutex_unlock(&bc->lock);
					return -1;
				}
				memcpy(snap[nkeys].key, e->key, e->key_len);
				snap[nkeys].key_len = e->key_len;
				nkeys++;
			}
		}
	}
	pthread_mutex_unlock(&bc->lock);

	/* Phase 2: dispatch callbacks without holding the lock. */
	for (i = 0; (size_t)i < nkeys; i++) {
		if (rc == 0 && fn(snap[i].key, snap[i].key_len, user) != 0)
			rc = 0; /* user requested early stop; not an error */
		free(snap[i].key);
	}
	free(snap);
	return 0;
}

int
bitcask_sync(bitcask_t *bc)
{
	if (bc == NULL) return -1;
	return fsync(bc->fd) == 0 ? 0 : -1;
}

void
bitcask_stat(bitcask_t *bc, bitcask_stats_t *out)
{
	if (bc == NULL || out == NULL) return;
	pthread_mutex_lock(&bc->lock);
	out->n_keys     = bc->n_keys;
	out->bytes_used = bc->bytes_used;
	out->bytes_dead = bc->bytes_dead;
	out->puts       = bc->puts;
	out->gets       = bc->gets;
	out->dels       = bc->dels;
	out->hits       = bc->hits;
	out->misses     = bc->misses;
	pthread_mutex_unlock(&bc->lock);
}
