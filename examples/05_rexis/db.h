/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/db.h
 *	Key-value database with xtc_lrlock protection.
 *	Supports string, list, and hash value types.
 */

#ifndef REXIS_DB_H
#define REXIS_DB_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_res.h"
#include "xtc_slab.h"
#include "xtc_lrlock.h"

/* Maximum key length */
#define DB_MAX_KEY_LEN   1024

/* Value types */
typedef enum db_val_type {
	DB_VAL_NONE   = 0,
	DB_VAL_STRING = 1,
	DB_VAL_LIST   = 2,
	DB_VAL_HASH   = 3
} db_val_type_t;

/* List node for list values */
typedef struct db_list_node {
	struct db_list_node *next;
	struct db_list_node *prev;
	size_t               len;
	char                 data[];
} db_list_node_t;

/* Hash field for hash values */
typedef struct db_hash_field {
	struct db_hash_field *next;
	size_t                key_len;
	size_t                val_len;
	char                  data[];   /* key then value */
} db_hash_field_t;

/* Database entry */
typedef struct db_entry {
	struct db_entry *next;         /* hash collision chain */
	uint64_t         hash;
	size_t           key_len;
	db_val_type_t    type;
	int64_t          expire_at_ns; /* 0 = no expiry */
	union {
		struct {
			char   *data;
			size_t  len;
		} str;
		struct {
			db_list_node_t *head;
			db_list_node_t *tail;
			size_t          count;
		} list;
		struct {
			db_hash_field_t *head;
			size_t           count;
		} hash;
	} val;
	char key[];
} db_entry_t;

/* Database configuration */
typedef struct db_opts {
	size_t       n_buckets;        /* hash table size; default 65536 */
	size_t       max_keys;         /* 0 = unlimited */
	int64_t      max_mem_bytes;    /* 0 = unlimited */
	xtc_res_t   *res;              /* optional resource accountant */
	const char  *persist_dir;      /* if non-NULL, enable Bitcask
	                                * persistence in this directory.
	                                * v1: only string SET/DEL are
	                                * logged.  Other types remain
	                                * memory-only. */
} db_opts_t;

#define DB_OPTS_DEFAULT { \
	.n_buckets     = 65536, \
	.max_keys      = 0, \
	.max_mem_bytes = 0, \
	.res           = NULL, \
	.persist_dir   = NULL \
}

/* Operation codes for xtc_lrlock apply_op */
typedef enum db_op_kind {
	DB_OP_SET = 1,
	DB_OP_DEL = 2,
	DB_OP_EXPIRE = 3,
	DB_OP_INCR = 4,
	DB_OP_LPUSH = 5,
	DB_OP_RPUSH = 6,
	DB_OP_LPOP = 7,
	DB_OP_RPOP = 8,
	DB_OP_HSET = 9,
	DB_OP_HDEL = 10,
	DB_OP_FLUSH = 11
} db_op_kind_t;

/* Database handle */
typedef struct db db_t;

/* Create/destroy */
int  db_create(const db_opts_t *opts, db_t **out);
void db_destroy(db_t *db);

/* ----- Read operations (wait-free under lrlock) ----- */

/* Begin/end read transaction.  Must be paired. */
void db_read_begin(db_t *db);
void db_read_end(db_t *db);

/* Returns NULL if not found or expired.  Only valid between
 * db_read_begin/end.  data/len are filled for string values. */
int  db_get(db_t *db, const char *key, size_t key_len,
            const char **data, size_t *len);

/* Check existence (1=exists, 0=not found) */
int  db_exists(db_t *db, const char *key, size_t key_len);

/* Get TTL in seconds (-2=not found, -1=no expiry, >=0=seconds left) */
int  db_ttl(db_t *db, const char *key, size_t key_len);

/* Get list length (0 if not found or not a list) */
size_t db_llen(db_t *db, const char *key, size_t key_len);

/* Get list range.  Fills out[] up to out_cap elements.
 * Returns count written.  start/stop are 0-based, can be negative. */
int db_lrange(db_t *db, const char *key, size_t key_len,
              int start, int stop,
              const char **out_data, size_t *out_len, int out_cap);

/* Hash operations (read) */
int    db_hget(db_t *db, const char *key, size_t key_len,
               const char *field, size_t field_len,
               const char **data, size_t *len);
size_t db_hlen(db_t *db, const char *key, size_t key_len);
int    db_hkeys(db_t *db, const char *key, size_t key_len,
                const char **out_keys, size_t *out_lens, int out_cap);
int    db_hvals(db_t *db, const char *key, size_t key_len,
                const char **out_vals, size_t *out_lens, int out_cap);
int    db_hgetall(db_t *db, const char *key, size_t key_len,
                  const char **out_keys, size_t *out_key_lens,
                  const char **out_vals, size_t *out_val_lens, int out_cap);

/* Enumerate keys matching glob pattern.
 * Returns count written.  Pattern supports * and ? wildcards. */
int db_keys(db_t *db, const char *pattern, size_t pattern_len,
            const char **out_keys, size_t *out_lens, int out_cap);

/* Statistics */
size_t db_key_count(db_t *db);
size_t db_mem_used(db_t *db);

/* ----- Write operations (serialized, applied to both copies) ----- */

/* Begin/end write transaction.  Must be paired.  Blocks until
 * the writer mutex is acquired. */
void db_write_begin(db_t *db);
void db_write_end(db_t *db);

/* Returns 0 on success, -1 on error (OOM, key limit, type mismatch). */
int  db_set(db_t *db, const char *key, size_t key_len,
            const char *val, size_t val_len);

/* Returns 0 on success, -1 on error */
int  db_set_ex(db_t *db, const char *key, size_t key_len,
               const char *val, size_t val_len, int64_t expire_ns);

/* Returns 1 if deleted, 0 if not found */
int  db_del(db_t *db, const char *key, size_t key_len);

/* Returns 0 on success, -1 if key not found */
int  db_expire(db_t *db, const char *key, size_t key_len, int64_t expire_ns);

/* Returns new value on success, -1 on type error */
int64_t db_incr(db_t *db, const char *key, size_t key_len, int64_t delta);

/* List operations.  Returns new list length, or -1 on error. */
int64_t db_lpush(db_t *db, const char *key, size_t key_len,
                 const char *val, size_t val_len);
int64_t db_rpush(db_t *db, const char *key, size_t key_len,
                 const char *val, size_t val_len);

/* Pop operations.  Returns 1 if popped, 0 if empty/not found.
 * Caller receives popped data via callback. */
typedef void (*db_pop_cb)(const char *data, size_t len, void *user);
int db_lpop(db_t *db, const char *key, size_t key_len,
            db_pop_cb cb, void *user);
int db_rpop(db_t *db, const char *key, size_t key_len,
            db_pop_cb cb, void *user);

/* Hash write operations.  Returns field count added (0 if exists). */
int db_hset(db_t *db, const char *key, size_t key_len,
            const char *field, size_t field_len,
            const char *val, size_t val_len);
int db_hdel(db_t *db, const char *key, size_t key_len,
            const char *field, size_t field_len);

/* Flush all keys */
void db_flushdb(db_t *db);

/* Expire keys that have passed their TTL.  Returns count removed. */
int db_expire_stale(db_t *db, int64_t now_ns, int max_scan);

#endif /* REXIS_DB_H */
