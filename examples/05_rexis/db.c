/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/db.c
 *	Key-value database with xtc_lrlock protection.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "db.h"
#include "xtc_sync.h"

/* Local helper wrapping xtc_clock_mono(). */
static inline int64_t xtc_now_ns(void) {
	int64_t t; t = xtc_clock_mono(); return t;
}

/* FNV-1a hash */
static uint64_t
fnv1a(const char *data, size_t len)
{
	uint64_t h = 14695981039346656037ULL;
	size_t i;
	for (i = 0; i < len; i++) {
		h ^= (uint64_t)(unsigned char)data[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/*
 * Hash table stored in the lrlock data.  The buckets are a linear hash
 * (lhash.h): one bucket split per insert past load factor 1, so chains
 * stay ~1 entry however many keys arrive.  The table used to be a FIXED
 * array of n_buckets (65,536) chains: at 1M keys every lookup walked
 * ~15 entries (12.7% of server CPU in db_lookup) and a 5M-key load ran
 * 3.7x slower than Valkey's.
 *
 * shortcut: the table never shrinks (DEL/FLUSHDB keep the peak's 8 B per
 * bucket); add an lh_shrink when a workload needs that memory back.
 */
typedef struct db_table {
	lh_t         lh;
	size_t       n_keys;         /* this shard's keys (load factor) */
	size_t       expire_cursor;  /* next bucket db_expire_stale scans */
} db_table_t;

#include "bitcask.h"

/*
 * Exclusion across loops.  The lrlock double-buffers only the table
 * HEADER (db_table_t); the entries, chains and values are one shared
 * copy that writers change in place.  On one loop that is safe because no
 * reader or writer parks inside its section, so they never overlap.  With
 * connections on several loops (OS threads) a reader on one thread could
 * walk a chain while a writer on another frees it, so a shared db also
 * holds a parking reader/writer latch (xtc_arwlock: a waiter parks its
 * fiber, FIFO, a writer is not starved) for every section.
 *
 * A shared db is DB_SHARDS shards, each its own table, lrlock and latch;
 * a key lives in the shard its hash's high bits pick (the table's bucket
 * uses the low bits).  A command latches only its keys' shards, in
 * ascending order (db_lock), so writes to different shards run in
 * parallel on different loops.  With one latch for the whole keyspace a
 * SET at --loops=2 ran at a quarter of the one-loop rate.  One loop: one
 * shard, no latch, as before.  The key count and the dataset bytes are
 * keyspace-wide (the budgets are): atomics, reserved before an insert.
 */
#ifndef DB_SHARDS
#define DB_SHARDS 64            /* a power of two, <= 64 (db_keyset_t) */
#endif
#if DB_SHARDS > 64 || (DB_SHARDS & (DB_SHARDS - 1)) != 0
#error "DB_SHARDS must be a power of two of at most 64"
#endif

struct db_shard {
	xtc_lrlock_t  *lr;
	xtc_arwlock_t *mt;         /* NULL unless opts.shared */
};

struct db {
	struct db_shard *sh;
	int           nshard;
	int           expire_shard;  /* db_expire_stale's current shard */
	_Atomic size_t keys;         /* live keys + in-flight reservations */
	_Atomic int64_t mem;         /* dataset bytes charged */
	db_opts_t     opts;
	bitcask_t    *bc;          /* persistence; NULL if disabled */
	_Atomic int   refused;     /* last db_malloc failed on the budget */
};

static struct db_shard *
shard_of(db_t *db, uint64_t h)
{
	return &db->sh[(size_t)(h >> 32) & (size_t)(db->nshard - 1)];
}

static const db_table_t *
rtable(db_t *db, uint64_t h)
{
	return xtc_lrlock_read_data(shard_of(db, h)->lr);
}

static db_table_t *
wtable(db_t *db, uint64_t h)
{
	return (db_table_t *)xtc_lrlock_write_data(shard_of(db, h)->lr);
}

/* One more key under --max-keys: reserved here, kept by the insert or
 * handed back (key_unreserve) when it fails. */
static int
key_reserve(db_t *db)
{
	size_t n = atomic_fetch_add(&db->keys, 1);
	if (db->opts.max_keys > 0 && n >= db->opts.max_keys) {
		atomic_fetch_sub(&db->keys, 1);
		return -1;
	}
	return 0;
}

static void
key_unreserve(db_t *db)
{
	atomic_fetch_sub(&db->keys, 1);
}

/* ----- Memory tracking ----- */

static void *
db_malloc(db_t *db, size_t sz)
{
	void *p;
	int64_t was = atomic_fetch_add(&db->mem, (int64_t)sz);
	if (db->opts.max_mem_bytes > 0 &&
	    was + (int64_t)sz > db->opts.max_mem_bytes) {
		atomic_fetch_sub(&db->mem, (int64_t)sz);
		db->refused = 1;
		return NULL;
	}
	if (db->opts.res) {
		if (xtc_res_acquire(db->opts.res, XTC_RES_MEM_BYTES,
		                    (int64_t)sz) != XTC_OK) {
			atomic_fetch_sub(&db->mem, (int64_t)sz);
			db->refused = 1;
			return NULL;
		}
	}
	p = xtc_malloc(sz);
	if (p == NULL) {
		atomic_fetch_sub(&db->mem, (int64_t)sz);
		if (db->opts.res)
			xtc_res_release(db->opts.res, XTC_RES_MEM_BYTES,
			    (int64_t)sz);
	}
	return p;
}

static void
db_uncharge(db_t *db, size_t sz)
{
	atomic_fetch_sub(&db->mem, (int64_t)sz);
	if (db->opts.res)
		xtc_res_release(db->opts.res, XTC_RES_MEM_BYTES, (int64_t)sz);
}

static void
db_free(db_t *db, void *p, size_t sz)
{
	if (!p)
		return;
	xtc_free(p);
	db_uncharge(db, sz);
}

/* ----- String values (DB_REF_MIN, db.h) -----
 *
 * A value of DB_REF_MIN bytes or more is a db_blob: a reference count in
 * front of the bytes, so e->val.str.data (and every reader of it) is
 * unchanged.  The keyspace holds one reference; a reply that sends the
 * bytes straight from here (cmd_get) holds another until they leave. */
typedef struct db_blob {
	_Atomic uint32_t refs;
	uint32_t         pad;
	uint64_t         pad2;      /* data stays 16-byte aligned */
	char             data[];
} db_blob_t;

static db_blob_t *
blob_of(const char *data)
{
	return (db_blob_t *)(void *)(data - offsetof(db_blob_t, data));
}

static size_t
val_size(size_t len)
{
	return len >= DB_REF_MIN ? sizeof(db_blob_t) + len + 1 : len + 1;
}

/* Room for a len-byte value (plus its NUL), charged; NULL if refused. */
static char *
val_alloc(db_t *db, size_t len)
{
	db_blob_t *b;

	if (len < DB_REF_MIN)
		return db_malloc(db, len + 1);
	if ((b = db_malloc(db, val_size(len))) == NULL)
		return NULL;
	atomic_init(&b->refs, 1);
	return b->data;
}

static void
blob_release(const char *data)
{
	db_blob_t *b = blob_of(data);

	if (atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) == 1)
		xtc_free(b);
}

/* The keyspace lets go of a value: its charge now, its memory once the
 * last holder has released it. */
static void
val_free(db_t *db, char *data, size_t len)
{
	if (data == NULL)
		return;
	if (len < DB_REF_MIN) {
		db_free(db, data, len + 1);
		return;
	}
	db_uncharge(db, val_size(len));
	blob_release(data);
}

/* REXIS_PLANT_NOREF is a planted bug for the test suite: holds and
 * releases do nothing, so a SET / DEL frees a value a parked reply is
 * still sending.  test_rexis_loopback /get_ref_outlives_overwrite must
 * fail with it (a heap-use-after-free under ASan, wrong bytes without). */
void
db_value_hold(const char *data)
{
#ifndef REXIS_PLANT_NOREF
	atomic_fetch_add_explicit(&blob_of(data)->refs, 1,
	    memory_order_relaxed);
#else
	(void)data;
#endif
}

void
db_value_release(const char *data)
{
#ifndef REXIS_PLANT_NOREF
	blob_release(data);
#else
	(void)data;
#endif
}

/* The table's own memory (directory + bucket segments) is keyspace
 * memory too: charged to the same budget, and a refusal only stops the
 * table growing (lhash.h). */
static void *
db_lh_alloc(void *ctx, size_t sz)
{
	db_t *db = ctx;
	void *p = db_malloc(db, sz);
	if (p != NULL)
		memset(p, 0, sz);
	return p;
}

static void
db_lh_free(void *ctx, void *p, size_t sz)
{
	db_t *db = ctx;
	db_free(db, p, sz);
}

/* ----- Entry helpers ----- */

static size_t
entry_size(size_t key_len)
{
	return sizeof(db_entry_t) + key_len + 1;
}

static db_entry_t *
entry_find(db_table_t *tbl, const char *key, size_t key_len, uint64_t h)
{
	lh_node_t *n;
	db_entry_t *e;
	for (n = *lh_slot(&tbl->lh, h); n; n = n->next) {
		e = (db_entry_t *)n;
		if (n->hash == h && e->key_len == key_len &&
		    memcmp(e->key, key, key_len) == 0)
			return e;
	}
	return NULL;
}

/* Link a new entry (key set, hash h; its key reserved, key_reserve) and
 * grow the shard's table past load 1. */
static void
entry_link(db_t *db, db_table_t *tbl, db_entry_t *e, uint64_t h)
{
	lh_node_t **slot = lh_slot(&tbl->lh, h);

	e->node.hash = h;
	e->node.next = *slot;
	*slot = &e->node;
	if (++tbl->n_keys > lh_buckets(&tbl->lh))
		(void)lh_grow(&tbl->lh, db_lh_alloc, db_lh_free, db);
}

static void
entry_free_value(db_t *db, db_entry_t *e)
{
	db_list_node_t *ln, *ln_next;
	db_hash_field_t *hf, *hf_next;

	switch (e->type) {
	case DB_VAL_STRING:
		val_free(db, e->val.str.data, e->val.str.len);
		break;
	case DB_VAL_LIST:
		for (ln = e->val.list.head; ln; ln = ln_next) {
			ln_next = ln->next;
			db_free(db, ln, sizeof(*ln) + ln->len + 1);
		}
		break;
	case DB_VAL_HASH:
		for (hf = e->val.hash.head; hf; hf = hf_next) {
			hf_next = hf->next;
			db_free(db, hf,
			        sizeof(*hf) + hf->key_len + hf->val_len + 2);
		}
		break;
	default:
		break;
	}
	e->type = DB_VAL_NONE;
}

static void
entry_remove(db_t *db, db_table_t *tbl, db_entry_t *e)
{
	lh_node_t **pp;

	for (pp = lh_slot(&tbl->lh, e->node.hash); *pp; pp = &(*pp)->next) {
		if (*pp == &e->node) {
			*pp = e->node.next;
			break;
		}
	}
	entry_free_value(db, e);
	db_free(db, e, entry_size(e->key_len));
	if (tbl->n_keys > 0)
		tbl->n_keys--;
	key_unreserve(db);
}

/* ----- lrlock callbacks ----- */

/* Op record for apply_op */
typedef struct db_op_hdr {
	db_op_kind_t kind;
	size_t       key_len;
	/* followed by key and op-specific payload */
} db_op_hdr_t;

static void
db_apply_op(void *data, const void *op, size_t op_size)
{
	/* This function is called from lrlock internals and cannot
	 * access the db_t handle directly.  For now we use a simple
	 * no-op since we're doing direct mutations in write_begin/end. */
	(void)data;
	(void)op;
	(void)op_size;
}

static void
db_sync(void *dst, const void *src, size_t sz)
{
	memcpy(dst, src, sz);
}

/* ----- Persistence (Bitcask) ----- */

/* Forward decl: db_set without a persistence write-back, used
 * during replay. */
static int db_set_inmem(db_t *, const char *, size_t,
                        const char *, size_t);

struct __replay_ctx {
	db_t      *db;
	bitcask_t *bc;
};

static int
__replay_one(const void *key, size_t key_len, void *user)
{
	struct __replay_ctx *ctx = user;
	size_t  vsz = 0;
	char   *val = NULL;
	int     rc;

	if (bitcask_size(ctx->bc, key, key_len, &vsz) != 0)
		return 0;
	val = malloc(vsz + 1);
	if (val == NULL) return 1;
	rc = bitcask_get(ctx->bc, key, key_len, val, vsz, &vsz);
	if (rc != 0) { free(val); return 1; }
	(void)db_set_inmem(ctx->db, key, key_len, val, vsz);
	free(val);
	return 0;
}

static void
db_persist_replay(db_t *db)
{
	struct __replay_ctx ctx = { db, db->bc };
	(void)bitcask_iterate(db->bc, __replay_one, &ctx);
}

/* ----- Public API ----- */

/* Free shards [0, n): their entries, tables, lrlocks and latches. */
static void
shards_destroy(db_t *db, int n)
{
	db_table_t *tbl;
	size_t i;
	lh_node_t *ln, *ln_next;
	db_entry_t *e;
	int k;

	for (k = 0; k < n; k++) {
		/* Free all entries in the write copy, then the table (both
		 * release their budget charge). */
		tbl = xtc_lrlock_write_begin(db->sh[k].lr);
		for (i = 0; tbl->lh.dir != NULL && i < lh_buckets(&tbl->lh);
		    i++) {
			for (ln = *lh_bucket(&tbl->lh, i); ln; ln = ln_next) {
				ln_next = ln->next;
				e = (db_entry_t *)ln;
				entry_free_value(db, e);
				db_free(db, e, entry_size(e->key_len));
			}
		}
		if (tbl->lh.dir != NULL)
			lh_destroy(&tbl->lh, db_lh_free, db);
		xtc_lrlock_write_end(db->sh[k].lr);
		xtc_lrlock_destroy(db->sh[k].lr);
		xtc_arwlock_destroy(db->sh[k].mt);
	}
	xtc_free(db->sh);
	db->sh = NULL;
}

/* Create shard k: its lrlock, latch (shared db) and empty table.  The
 * bucket table is charged to the shared budget (db_lh_alloc), so the
 * budget's total is exactly what INFO reports as used_memory (keyspace
 * + client buffers).  A cap below it cannot hold an empty keyspace:
 * XTC_E_RESOURCE. */
static int
shard_init(db_t *db, int k)
{
	xtc_lrlock_opts_t lr_opts = { 0 };
	struct db_shard *sh = &db->sh[k];
	db_table_t *tbl;
	size_t nb = db->opts.n_buckets / (size_t)db->nshard;

	lr_opts.name = "rexis_db";
	lr_opts.data_size = sizeof(db_table_t);
	lr_opts.apply_fn = db_apply_op;
	lr_opts.sync_fn = db_sync;
	lr_opts.oplog_capacity = 64;     /* no ops are logged (db_apply_op) */
	if (xtc_lrlock_create_ex(&lr_opts, &sh->lr) != XTC_OK)
		return XTC_E_NOMEM;
	if (db->opts.shared && xtc_arwlock_create(&sh->mt) != XTC_OK) {
		xtc_lrlock_destroy(sh->lr);
		return XTC_E_NOMEM;
	}
	tbl = xtc_lrlock_write_begin(sh->lr);
	memset(tbl, 0, sizeof(*tbl));
	if (lh_init(&tbl->lh, nb, db_lh_alloc, db_lh_free, db) != 0) {
		xtc_lrlock_write_end(sh->lr);
		xtc_lrlock_destroy(sh->lr);
		xtc_arwlock_destroy(sh->mt);
		return db->refused ? XTC_E_RESOURCE : XTC_E_NOMEM;
	}
	xtc_lrlock_publish_full_sync(sh->lr);
	xtc_lrlock_write_end(sh->lr);

	/* publish_full_sync copied the table struct into the other copy, so
	 * both copies now share this one bucket table (every write_end does
	 * the same full sync).  A second table used to be allocated here and
	 * the next full sync overwrote one copy's pointer with the other's,
	 * leaking 512 KiB per db_create (ASan, test_db_persist). */

	xtc_lrlock_mark_ready(sh->lr);
	return XTC_OK;
}

int
db_create(const db_opts_t *opts, db_t **out)
{
	db_t *db;
	int rc, k;

	if (!out)
		return XTC_E_INVAL;

	if ((db = xtc_calloc(1, sizeof(*db))) == NULL)
		return XTC_E_NOMEM;

	db->opts = opts ? *opts : (db_opts_t)DB_OPTS_DEFAULT;
	if (db->opts.n_buckets == 0)
		db->opts.n_buckets = 65536;
	db->nshard = db->opts.shared ? DB_SHARDS : 1;
	if ((db->sh = xtc_calloc((size_t)db->nshard, sizeof *db->sh)) == NULL) {
		xtc_free(db);
		return XTC_E_NOMEM;
	}
	for (k = 0; k < db->nshard; k++) {
		if ((rc = shard_init(db, k)) != XTC_OK) {
			shards_destroy(db, k);
			xtc_free(db);
			return rc;
		}
	}

	/* Open Bitcask for persistence if configured. */
	if (db->opts.persist_dir != NULL) {
		if (bitcask_open(db->opts.persist_dir, &db->bc) != 0) {
			shards_destroy(db, db->nshard);
			xtc_free(db);
			return XTC_E_INTERNAL;
		}
		/* Replay live keys from the bitcask into the in-memory
		 * table.  We use bitcask_iterate to visit each key; for
		 * each, we read the value and call db_set without writing
		 * back to bitcask.  The replay mutates the write copy, so
		 * it is a write transaction like any other: without the
		 * publish in db_write_end the readers kept the empty
		 * header (DBSIZE 0 after every restart until the first
		 * write command). */
		db_write_begin(db);
		db_persist_replay(db);
		db_write_end(db);
	}

	*out = db;
	return XTC_OK;
}

void
db_destroy(db_t *db)
{
	if (!db)
		return;
	shards_destroy(db, db->nshard);
	if (db->bc != NULL) bitcask_close(db->bc);
	xtc_free(db);
}

db_keyset_t
db_key_shard(db_t *db, const char *key, size_t key_len)
{
	if (db->nshard == 1)
		return 1;
	return (db_keyset_t)1 << ((fnv1a(key, key_len) >> 32) &
	    (uint64_t)(db->nshard - 1));
}

/* Index of the lowest set bit of a non-zero set. */
static int
set_low(db_keyset_t set)
{
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_ctzll(set);
#else
	int k = 0;
	while ((set & 1) == 0) {
		set >>= 1;
		k++;
	}
	return k;
#endif
}

/* Latch the shards of `set` (shared db), then enter their lrlock
 * sections, in ascending shard order: two commands that need
 * overlapping sets cannot deadlock. */
void
db_lock(db_t *db, db_keyset_t set, int write)
{
	int k;

	if (db->nshard < 64)
		set &= ((db_keyset_t)1 << db->nshard) - 1;
	for (; set != 0; set &= set - 1) {
		k = set_low(set);
		if (db->sh[k].mt != NULL)
			(void)(write ? xtc_arwlock_wrlock(db->sh[k].mt, -1) :
			    xtc_arwlock_rdlock(db->sh[k].mt, -1));
		if (write)
			(void)xtc_lrlock_write_begin(db->sh[k].lr);
		else
			(void)xtc_lrlock_read_begin(db->sh[k].lr);
	}
}

void
db_unlock(db_t *db, db_keyset_t set, int write)
{
	int k;

	if (db->nshard < 64)
		set &= ((db_keyset_t)1 << db->nshard) - 1;
	for (; set != 0; set &= set - 1) {      /* any order releases */
		k = set_low(set);
		if (write) {
			xtc_lrlock_publish_full_sync(db->sh[k].lr);
			xtc_lrlock_write_end(db->sh[k].lr);
		} else {
			xtc_lrlock_read_end(db->sh[k].lr);
		}
		if (db->sh[k].mt != NULL)
			(void)xtc_arwlock_unlock(db->sh[k].mt);
	}
}

void
db_read_begin(db_t *db)
{
	db_lock(db, DB_ALL_SHARDS, 0);
}

void
db_read_end(db_t *db)
{
	db_unlock(db, DB_ALL_SHARDS, 0);
}

void
db_write_begin(db_t *db)
{
	db_lock(db, DB_ALL_SHARDS, 1);
}

void
db_write_end(db_t *db)
{
	db_unlock(db, DB_ALL_SHARDS, 1);
}

/* ----- Read operations ----- */

static const db_entry_t *
db_lookup(db_t *db, const char *key, size_t key_len)
{
	uint64_t h = fnv1a(key, key_len);
	const db_table_t *tbl = rtable(db, h);
	const lh_node_t *n;
	const db_entry_t *e;

	for (n = *lh_slot(&tbl->lh, h); n; n = n->next) {
		e = (const db_entry_t *)n;
		if (n->hash == h && e->key_len == key_len &&
		    memcmp(e->key, key, key_len) == 0)
			return e;
	}
	return NULL;
}

int
db_get(db_t *db, const char *key, size_t key_len,
       const char **data, size_t *len)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	if (!e || e->type != DB_VAL_STRING)
		return -1;
	if (e->expire_at_ns > 0) {
		int64_t now = xtc_now_ns();
		if (now >= e->expire_at_ns)
			return -1;
	}
	*data = e->val.str.data;
	*len = e->val.str.len;
	return 0;
}

int
db_exists(db_t *db, const char *key, size_t key_len)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	if (!e)
		return 0;
	if (e->expire_at_ns > 0 && xtc_now_ns() >= e->expire_at_ns)
		return 0;
	return 1;
}

int
db_ttl(db_t *db, const char *key, size_t key_len)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	int64_t now;

	if (!e)
		return -2;
	if (e->expire_at_ns == 0)
		return -1;
	now = xtc_now_ns();
	if (now >= e->expire_at_ns)
		return -2;
	return (int)((e->expire_at_ns - now) / (1000LL * 1000 * 1000));
}

size_t
db_llen(db_t *db, const char *key, size_t key_len)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	if (!e || e->type != DB_VAL_LIST)
		return 0;
	if (e->expire_at_ns > 0 && xtc_now_ns() >= e->expire_at_ns)
		return 0;
	return e->val.list.count;
}

size_t
db_lrange(db_t *db, const char *key, size_t key_len,
          int64_t start, int64_t stop, db_visit_fn fn, void *user)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_list_node_t *n;
	int64_t count, idx;

	if (!e || e->type != DB_VAL_LIST ||
	    (e->expire_at_ns > 0 && xtc_now_ns() >= e->expire_at_ns)) {
		fn(NULL, 0, NULL, 0, user);
		return 0;
	}

	count = (int64_t)e->val.list.count;
	if (start < 0) start = count + start;
	if (stop < 0)  stop = count + stop;
	if (start < 0) start = 0;
	if (stop >= count) stop = count - 1;
	if (start > stop)
		start = stop + 1;   /* empty range: still announce 0 */
	fn(NULL, (size_t)(stop - start + 1), NULL, 0, user);

	idx = 0;
	for (n = e->val.list.head; n && idx <= stop; n = n->next, idx++)
		if (idx >= start)
			fn(n->data, n->len, NULL, 0, user);
	return (size_t)(stop - start + 1);
}

int
db_hget(db_t *db, const char *key, size_t key_len,
        const char *field, size_t field_len,
        const char **data, size_t *len)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_hash_field_t *f;

	if (!e || e->type != DB_VAL_HASH)
		return -1;
	if (e->expire_at_ns > 0 && xtc_now_ns() >= e->expire_at_ns)
		return -1;

	for (f = e->val.hash.head; f; f = f->next) {
		if (f->key_len == field_len &&
		    memcmp(f->data, field, field_len) == 0) {
			*data = f->data + f->key_len + 1;
			*len = f->val_len;
			return 0;
		}
	}
	return -1;
}

size_t
db_hlen(db_t *db, const char *key, size_t key_len)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	if (!e || e->type != DB_VAL_HASH)
		return 0;
	if (e->expire_at_ns > 0 && xtc_now_ns() >= e->expire_at_ns)
		return 0;
	return e->val.hash.count;
}

size_t
db_hscan(db_t *db, const char *key, size_t key_len,
         db_visit_fn fn, void *user)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_hash_field_t *f;

	if (!e || e->type != DB_VAL_HASH ||
	    (e->expire_at_ns > 0 && xtc_now_ns() >= e->expire_at_ns)) {
		fn(NULL, 0, NULL, 0, user);
		return 0;
	}
	fn(NULL, e->val.hash.count, NULL, 0, user);
	for (f = e->val.hash.head; f; f = f->next)
		fn(f->data, f->key_len, f->data + f->key_len + 1, f->val_len,
		    user);
	return e->val.hash.count;
}

/* Simple glob matching: * matches any sequence, ? matches one char */
static int
glob_match(const char *pattern, size_t plen, const char *str, size_t slen)
{
	size_t pi = 0, si = 0;
	size_t star_pi = (size_t)-1, star_si = 0;

	while (si < slen) {
		if (pi < plen && (pattern[pi] == str[si] || pattern[pi] == '?')) {
			pi++; si++;
		} else if (pi < plen && pattern[pi] == '*') {
			star_pi = pi++;
			star_si = si;
		} else if (star_pi != (size_t)-1) {
			pi = star_pi + 1;
			si = ++star_si;
		} else {
			return 0;
		}
	}
	while (pi < plen && pattern[pi] == '*')
		pi++;
	return pi == plen;
}

size_t
db_keys(db_t *db, const char *pattern, size_t pattern_len,
        db_visit_fn fn, void *user)
{
	const db_table_t *tbl;
	int64_t now = xtc_now_ns();
	const lh_node_t *n;
	const db_entry_t *e;
	size_t i, count = 0;
	int pass, k;

	/* Pass 0 counts, pass 1 visits: same snapshot (every shard is held,
	 * db_read_begin), same `now`, so the announced count is exactly the
	 * number visited. */
	for (pass = 0; pass < 2; pass++) {
		if (pass == 1)
			fn(NULL, count, NULL, 0, user);
		for (k = 0; k < db->nshard; k++)
		for (tbl = xtc_lrlock_read_data(db->sh[k].lr), i = 0;
		    i < lh_buckets(&tbl->lh); i++) {
			for (n = *lh_bucket(&tbl->lh, i); n; n = n->next) {
				e = (const db_entry_t *)n;
				if (e->expire_at_ns > 0 && now >= e->expire_at_ns)
					continue;
				if (!glob_match(pattern, pattern_len, e->key,
				    e->key_len))
					continue;
				if (pass == 0)
					count++;
				else
					fn(e->key, e->key_len, NULL, 0, user);
			}
		}
	}
	return count;
}

/* Keyspace-wide counters: no section needed (atomics). */
size_t
db_key_count(db_t *db)
{
	return atomic_load(&db->keys);
}

size_t
db_mem_used(db_t *db)
{
	int64_t m = atomic_load(&db->mem);
	return m > 0 ? (size_t)m : 0;
}

/* ----- Write operations ----- */

int
db_set(db_t *db, const char *key, size_t key_len,
       const char *val, size_t val_len)
{
	return db_set_ex(db, key, key_len, val, val_len, 0);
}

static int
db_set_inmem(db_t *db, const char *key, size_t key_len,
             const char *val, size_t val_len)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e;
	char *val_copy;

	if (key_len > DB_MAX_KEY_LEN)
		return -1;

	e = entry_find(tbl, key, key_len, h);
	if (e) {
		/* Replace existing */
		entry_free_value(db, e);
		val_copy = val_alloc(db, val_len);
		if (!val_copy)
			return -1;
		memcpy(val_copy, val, val_len);
		val_copy[val_len] = '\0';
		e->type = DB_VAL_STRING;
		e->val.str.data = val_copy;
		e->val.str.len = val_len;
		e->expire_at_ns = 0;
		return 0;
	}

	/* Check key limit */
	if (key_reserve(db) != 0)
		return -1;

	/* New entry */
	e = db_malloc(db, entry_size(key_len));
	if (!e) {
		key_unreserve(db);
		return -1;
	}

	val_copy = val_alloc(db, val_len);
	if (!val_copy) {
		db_free(db, e, entry_size(key_len));
		key_unreserve(db);
		return -1;
	}

	memcpy(val_copy, val, val_len);
	val_copy[val_len] = '\0';
	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->type = DB_VAL_STRING;
	e->expire_at_ns = 0;
	e->val.str.data = val_copy;
	e->val.str.len = val_len;

	entry_link(db, tbl, e, h);

	return 0;
}

int
db_set_ex(db_t *db, const char *key, size_t key_len,
          const char *val, size_t val_len, int64_t expire_ns)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e;
	char *val_copy;

	if (key_len > DB_MAX_KEY_LEN)
		return -1;

	e = entry_find(tbl, key, key_len, h);
	if (e) {
		/* Replace existing */
		entry_free_value(db, e);
		val_copy = val_alloc(db, val_len);
		if (!val_copy)
			return -1;
		memcpy(val_copy, val, val_len);
		val_copy[val_len] = '\0';
		e->type = DB_VAL_STRING;
		e->val.str.data = val_copy;
		e->val.str.len = val_len;
		e->expire_at_ns = expire_ns;
		goto persist;
	}

	/* Check key limit */
	if (key_reserve(db) != 0)
		return -1;

	/* New entry */
	e = db_malloc(db, entry_size(key_len));
	if (!e) {
		key_unreserve(db);
		return -1;
	}

	val_copy = val_alloc(db, val_len);
	if (!val_copy) {
		db_free(db, e, entry_size(key_len));
		key_unreserve(db);
		return -1;
	}

	memcpy(val_copy, val, val_len);
	val_copy[val_len] = '\0';
	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->type = DB_VAL_STRING;
	e->expire_at_ns = expire_ns;
	e->val.str.data = val_copy;
	e->val.str.len = val_len;

	entry_link(db, tbl, e, h);

persist:
	/* Persist to bitcask if enabled.  TTLs are not persisted in v1
	 * (the entry comes back without expire_at_ns after a restart);
	 * documented in db.h. */
	if (db->bc != NULL && expire_ns == 0) {
		(void)bitcask_put(db->bc, key, key_len, val, val_len);
	}
	return 0;
}

int
db_del(db_t *db, const char *key, size_t key_len)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);

	if (!e)
		return 0;
	entry_remove(db, tbl, e);

	if (db->bc != NULL) {
		(void)bitcask_del(db->bc, key, key_len);
	}
	return 1;
}

int
db_expire(db_t *db, const char *key, size_t key_len, int64_t expire_ns)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);

	if (!e)
		return -1;
	e->expire_at_ns = expire_ns;
	return 0;
}

int64_t
db_incr(db_t *db, const char *key, size_t key_len, int64_t delta)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	int64_t val;
	char buf[32];
	int n;

	if (!e) {
		/* Create new key with value=delta */
		n = snprintf(buf, sizeof buf, "%lld", (long long)delta);
		if (db_set(db, key, key_len, buf, (size_t)n) < 0)
			return -1;
		return delta;
	}

	if (e->type != DB_VAL_STRING)
		return -1;

	/* Parse existing value */
	val = 0;
	{
		const char *s = e->val.str.data;
		size_t slen = e->val.str.len;
		int neg = 0;
		size_t i = 0;

		if (slen > 0 && s[0] == '-') {
			neg = 1;
			i = 1;
		}
		for (; i < slen; i++) {
			if (s[i] < '0' || s[i] > '9')
				return -1;
			val = val * 10 + (s[i] - '0');
		}
		if (neg) val = -val;
	}

	val += delta;

	/* Update */
	n = snprintf(buf, sizeof buf, "%lld", (long long)val);
	val_free(db, e->val.str.data, e->val.str.len);
	e->val.str.data = db_malloc(db, (size_t)n + 1);   /* < DB_REF_MIN */
	if (!e->val.str.data) {
		/* Entry is now corrupted; remove it */
		entry_remove(db, tbl, e);
		return -1;
	}
	memcpy(e->val.str.data, buf, (size_t)n + 1);
	e->val.str.len = (size_t)n;

	return val;
}

/* ----- List operations ----- */

static db_entry_t *
db_get_or_create_list(db_t *db, const char *key, size_t key_len)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);

	if (e) {
		if (e->type != DB_VAL_LIST)
			return NULL;
		return e;
	}

	/* Check key limit */
	if (key_reserve(db) != 0)
		return NULL;

	e = db_malloc(db, entry_size(key_len));
	if (!e) {
		key_unreserve(db);
		return NULL;
	}

	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->type = DB_VAL_LIST;
	e->expire_at_ns = 0;
	e->val.list.head = NULL;
	e->val.list.tail = NULL;
	e->val.list.count = 0;

	entry_link(db, tbl, e, h);

	return e;
}

int64_t
db_lpush(db_t *db, const char *key, size_t key_len,
         const char *val, size_t val_len)
{
	db_entry_t *e = db_get_or_create_list(db, key, key_len);
	db_list_node_t *n;

	if (!e)
		return -1;

	n = db_malloc(db, sizeof(*n) + val_len + 1);
	if (!n)
		return -1;

	memcpy(n->data, val, val_len);
	n->data[val_len] = '\0';
	n->len = val_len;
	n->prev = NULL;
	n->next = e->val.list.head;
	if (e->val.list.head)
		e->val.list.head->prev = n;
	else
		e->val.list.tail = n;
	e->val.list.head = n;
	e->val.list.count++;

	return (int64_t)e->val.list.count;
}

int64_t
db_rpush(db_t *db, const char *key, size_t key_len,
         const char *val, size_t val_len)
{
	db_entry_t *e = db_get_or_create_list(db, key, key_len);
	db_list_node_t *n;

	if (!e)
		return -1;

	n = db_malloc(db, sizeof(*n) + val_len + 1);
	if (!n)
		return -1;

	memcpy(n->data, val, val_len);
	n->data[val_len] = '\0';
	n->len = val_len;
	n->next = NULL;
	n->prev = e->val.list.tail;
	if (e->val.list.tail)
		e->val.list.tail->next = n;
	else
		e->val.list.head = n;
	e->val.list.tail = n;
	e->val.list.count++;

	return (int64_t)e->val.list.count;
}

int
db_lpop(db_t *db, const char *key, size_t key_len,
        db_pop_cb cb, void *user)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	db_list_node_t *n;

	if (!e || e->type != DB_VAL_LIST || !e->val.list.head)
		return 0;

	n = e->val.list.head;
	e->val.list.head = n->next;
	if (e->val.list.head)
		e->val.list.head->prev = NULL;
	else
		e->val.list.tail = NULL;
	e->val.list.count--;

	if (cb)
		cb(n->data, n->len, user);
	db_free(db, n, sizeof(*n) + n->len + 1);

	/* Remove empty list */
	if (e->val.list.count == 0)
		entry_remove(db, tbl, e);

	return 1;
}

int
db_rpop(db_t *db, const char *key, size_t key_len,
        db_pop_cb cb, void *user)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	db_list_node_t *n;

	if (!e || e->type != DB_VAL_LIST || !e->val.list.tail)
		return 0;

	n = e->val.list.tail;
	e->val.list.tail = n->prev;
	if (e->val.list.tail)
		e->val.list.tail->next = NULL;
	else
		e->val.list.head = NULL;
	e->val.list.count--;

	if (cb)
		cb(n->data, n->len, user);
	db_free(db, n, sizeof(*n) + n->len + 1);

	/* Remove empty list */
	if (e->val.list.count == 0)
		entry_remove(db, tbl, e);

	return 1;
}

/* ----- Hash operations ----- */

static db_entry_t *
db_get_or_create_hash(db_t *db, const char *key, size_t key_len)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);

	if (e) {
		if (e->type != DB_VAL_HASH)
			return NULL;
		return e;
	}

	/* Check key limit */
	if (key_reserve(db) != 0)
		return NULL;

	e = db_malloc(db, entry_size(key_len));
	if (!e) {
		key_unreserve(db);
		return NULL;
	}

	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->type = DB_VAL_HASH;
	e->expire_at_ns = 0;
	e->val.hash.head = NULL;
	e->val.hash.count = 0;

	entry_link(db, tbl, e, h);

	return e;
}

int
db_hset(db_t *db, const char *key, size_t key_len,
        const char *field, size_t field_len,
        const char *val, size_t val_len)
{
	db_entry_t *e = db_get_or_create_hash(db, key, key_len);
	db_hash_field_t *f, *prev = NULL;
	int added = 0;

	if (!e)
		return -1;

	/* Check if field exists */
	for (f = e->val.hash.head; f; prev = f, f = f->next) {
		if (f->key_len == field_len &&
		    memcmp(f->data, field, field_len) == 0) {
			/* Replace value */
			db_hash_field_t *nf = db_malloc(db,
			    sizeof(*nf) + field_len + val_len + 2);
			if (!nf)
				return -1;
			nf->key_len = field_len;
			nf->val_len = val_len;
			memcpy(nf->data, field, field_len);
			nf->data[field_len] = '\0';
			memcpy(nf->data + field_len + 1, val, val_len);
			nf->data[field_len + 1 + val_len] = '\0';
			nf->next = f->next;
			if (prev)
				prev->next = nf;
			else
				e->val.hash.head = nf;
			db_free(db, f,
			        sizeof(*f) + f->key_len + f->val_len + 2);
			return 0;
		}
	}

	/* New field */
	f = db_malloc(db, sizeof(*f) + field_len + val_len + 2);
	if (!f)
		return -1;
	f->key_len = field_len;
	f->val_len = val_len;
	memcpy(f->data, field, field_len);
	f->data[field_len] = '\0';
	memcpy(f->data + field_len + 1, val, val_len);
	f->data[field_len + 1 + val_len] = '\0';
	f->next = e->val.hash.head;
	e->val.hash.head = f;
	e->val.hash.count++;
	added = 1;

	return added;
}

int
db_hdel(db_t *db, const char *key, size_t key_len,
        const char *field, size_t field_len)
{
	uint64_t h = fnv1a(key, key_len);
	db_table_t *tbl = wtable(db, h);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	db_hash_field_t **pp, *f;

	if (!e || e->type != DB_VAL_HASH)
		return 0;

	for (pp = &e->val.hash.head; *pp; pp = &(*pp)->next) {
		f = *pp;
		if (f->key_len == field_len &&
		    memcmp(f->data, field, field_len) == 0) {
			*pp = f->next;
			db_free(db, f,
			        sizeof(*f) + f->key_len + f->val_len + 2);
			e->val.hash.count--;
			/* Remove empty hash */
			if (e->val.hash.count == 0)
				entry_remove(db, tbl, e);
			return 1;
		}
	}
	return 0;
}

void
db_flushdb(db_t *db)
{
	db_table_t *tbl;
	size_t i;
	lh_node_t *n, *n_next;
	db_entry_t *e;
	int k;

	for (k = 0; k < db->nshard; k++) {
		tbl = xtc_lrlock_write_data(db->sh[k].lr);
		for (i = 0; i < lh_buckets(&tbl->lh); i++) {
			for (n = *lh_bucket(&tbl->lh, i); n; n = n_next) {
				n_next = n->next;
				e = (db_entry_t *)n;
				entry_free_value(db, e);
				db_free(db, e, entry_size(e->key_len));
			}
			*lh_bucket(&tbl->lh, i) = NULL;
		}
		atomic_fetch_sub(&db->keys, tbl->n_keys);
		tbl->n_keys = 0;
	}
}

int
db_expire_stale(db_t *db, int64_t now_ns, int max_scan)
{
	db_table_t *tbl;
	int removed = 0, scanned = 0, k, sh;
	size_t i, visited, nb;

	/* Resume where the last pass stopped and wrap, so successive passes
	 * sweep the whole table (Redis's active-expire cursor; DragonFly's
	 * DbTable::expire_cursor).  Restarting at bucket 0 every time kept
	 * rescanning the same first max_scan entries: expired keys behind
	 * them were never reclaimed.  A bucket is always finished, so a pass
	 * may look at up to one chain more than max_scan.  The cursor walks
	 * one shard, then the next. */
	for (k = 0; k < db->nshard && scanned < max_scan; k++) {
		sh = (db->expire_shard + k) % db->nshard;
		tbl = xtc_lrlock_write_data(db->sh[sh].lr);
		nb = lh_buckets(&tbl->lh);
		i = tbl->expire_cursor % nb;
		for (visited = 0; visited < nb && scanned < max_scan;
		    visited++, i = (i + 1) % nb) {
			lh_node_t **pp = lh_bucket(&tbl->lh, i);
			while (*pp) {
				db_entry_t *e = (db_entry_t *)*pp;
				scanned++;
				if (e->expire_at_ns > 0 &&
				    now_ns >= e->expire_at_ns) {
					*pp = e->node.next;
					entry_free_value(db, e);
					db_free(db, e, entry_size(e->key_len));
					if (tbl->n_keys > 0)
						tbl->n_keys--;
					key_unreserve(db);
					removed++;
				} else {
					pp = &e->node.next;
				}
			}
		}
		tbl->expire_cursor = i;
		db->expire_shard = sh;
		if (visited == nb)           /* swept it: the next shard */
			db->expire_shard = (sh + 1) % db->nshard;
	}
	return removed;
}
