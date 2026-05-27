/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_redis/db.c
 *	Key-value database with xtc_lrlock protection.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "db.h"
#include "xtc_int.h"

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

/* Hash table stored in the lrlock data */
typedef struct db_table {
	size_t       n_buckets;
	size_t       n_keys;
	size_t       mem_used;
	db_entry_t **buckets;
} db_table_t;

struct db {
	xtc_lrlock_t *lr;
	db_opts_t     opts;
};

/* ----- Memory tracking ----- */

static void *
db_malloc(db_t *db, db_table_t *tbl, size_t sz)
{
	void *p;
	if (db->opts.max_mem_bytes > 0 &&
	    tbl->mem_used + sz > (size_t)db->opts.max_mem_bytes)
		return NULL;
	if (db->opts.res) {
		if (xtc_res_acquire(db->opts.res, XTC_RES_MEM_BYTES,
		                    (int64_t)sz) != XTC_OK)
			return NULL;
	}
	p = __os_malloc(sz);
	if (p)
		tbl->mem_used += sz;
	else if (db->opts.res)
		xtc_res_release(db->opts.res, XTC_RES_MEM_BYTES, (int64_t)sz);
	return p;
}

static void
db_free(db_t *db, db_table_t *tbl, void *p, size_t sz)
{
	if (!p)
		return;
	__os_free(p);
	if (tbl->mem_used >= sz)
		tbl->mem_used -= sz;
	if (db->opts.res)
		xtc_res_release(db->opts.res, XTC_RES_MEM_BYTES, (int64_t)sz);
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
	db_entry_t *e;
	size_t idx = h % tbl->n_buckets;
	for (e = tbl->buckets[idx]; e; e = e->next) {
		if (e->hash == h && e->key_len == key_len &&
		    memcmp(e->key, key, key_len) == 0)
			return e;
	}
	return NULL;
}

static void
entry_free_value(db_t *db, db_table_t *tbl, db_entry_t *e)
{
	db_list_node_t *ln, *ln_next;
	db_hash_field_t *hf, *hf_next;

	switch (e->type) {
	case DB_VAL_STRING:
		db_free(db, tbl, e->val.str.data, e->val.str.len + 1);
		break;
	case DB_VAL_LIST:
		for (ln = e->val.list.head; ln; ln = ln_next) {
			ln_next = ln->next;
			db_free(db, tbl, ln, sizeof(*ln) + ln->len + 1);
		}
		break;
	case DB_VAL_HASH:
		for (hf = e->val.hash.head; hf; hf = hf_next) {
			hf_next = hf->next;
			db_free(db, tbl, hf,
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
	size_t idx = e->hash % tbl->n_buckets;
	db_entry_t **pp;

	for (pp = &tbl->buckets[idx]; *pp; pp = &(*pp)->next) {
		if (*pp == e) {
			*pp = e->next;
			break;
		}
	}
	entry_free_value(db, tbl, e);
	db_free(db, tbl, e, entry_size(e->key_len));
	if (tbl->n_keys > 0)
		tbl->n_keys--;
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

/* ----- Public API ----- */

int
db_create(const db_opts_t *opts, db_t **out)
{
	db_t *db;
	xtc_lrlock_opts_t lr_opts = { 0 };
	db_table_t *tbl;
	size_t bucket_bytes;

	if (!out)
		return XTC_E_INVAL;

	db = __os_calloc(1, sizeof(*db));
	if (!db)
		return XTC_E_NOMEM;

	db->opts = opts ? *opts : (db_opts_t)DB_OPTS_DEFAULT;
	if (db->opts.n_buckets == 0)
		db->opts.n_buckets = 65536;

	lr_opts.name = "redis_db";
	lr_opts.data_size = sizeof(db_table_t);
	lr_opts.apply_fn = db_apply_op;
	lr_opts.sync_fn = db_sync;

	if (xtc_lrlock_create_ex(&lr_opts, &db->lr) != XTC_OK) {
		__os_free(db);
		return XTC_E_NOMEM;
	}

	/* Initialize the table in the write copy, then mark ready */
	tbl = xtc_lrlock_write_begin(db->lr);
	tbl->n_buckets = db->opts.n_buckets;
	tbl->n_keys = 0;
	tbl->mem_used = 0;
	bucket_bytes = tbl->n_buckets * sizeof(db_entry_t *);
	tbl->buckets = __os_calloc(tbl->n_buckets, sizeof(db_entry_t *));
	if (!tbl->buckets) {
		xtc_lrlock_write_end(db->lr);
		xtc_lrlock_destroy(db->lr);
		__os_free(db);
		return XTC_E_NOMEM;
	}
	tbl->mem_used = bucket_bytes;
	xtc_lrlock_publish_full_sync(db->lr);
	xtc_lrlock_write_end(db->lr);

	/* Allocate buckets for the second copy too */
	tbl = xtc_lrlock_write_begin(db->lr);
	/* After full_sync the read copy has buckets = NULL; we need to
	 * allocate separately for the now-write copy. */
	tbl->buckets = __os_calloc(tbl->n_buckets, sizeof(db_entry_t *));
	if (!tbl->buckets) {
		xtc_lrlock_write_end(db->lr);
		/* Clean up the other copy's buckets */
		xtc_lrlock_destroy(db->lr);
		__os_free(db);
		return XTC_E_NOMEM;
	}
	xtc_lrlock_publish_full_sync(db->lr);
	xtc_lrlock_write_end(db->lr);

	xtc_lrlock_mark_ready(db->lr);

	*out = db;
	return XTC_OK;
}

void
db_destroy(db_t *db)
{
	db_table_t *tbl;
	size_t i;
	db_entry_t *e, *e_next;

	if (!db)
		return;

	/* Free all entries in the write copy */
	tbl = xtc_lrlock_write_begin(db->lr);
	if (tbl->buckets) {
		for (i = 0; i < tbl->n_buckets; i++) {
			for (e = tbl->buckets[i]; e; e = e_next) {
				e_next = e->next;
				entry_free_value(db, tbl, e);
				db_free(db, tbl, e, entry_size(e->key_len));
			}
		}
		__os_free(tbl->buckets);
	}
	xtc_lrlock_write_end(db->lr);

	xtc_lrlock_destroy(db->lr);
	__os_free(db);
}

void db_read_begin(db_t *db) { (void)xtc_lrlock_read_begin(db->lr); }
void db_read_end(db_t *db)   { xtc_lrlock_read_end(db->lr); }
void db_write_begin(db_t *db){ (void)xtc_lrlock_write_begin(db->lr); }

void
db_write_end(db_t *db)
{
	xtc_lrlock_publish_full_sync(db->lr);
	xtc_lrlock_write_end(db->lr);
}

/* ----- Read operations ----- */

static const db_entry_t *
db_lookup(db_t *db, const char *key, size_t key_len)
{
	const db_table_t *tbl = xtc_lrlock_read_data(db->lr);
	uint64_t h = fnv1a(key, key_len);
	const db_entry_t *e;
	size_t idx = h % tbl->n_buckets;

	for (e = tbl->buckets[idx]; e; e = e->next) {
		if (e->hash == h && e->key_len == key_len &&
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
		int64_t now = __os_clock_mono();
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
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
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
	now = __os_clock_mono();
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
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
		return 0;
	return e->val.list.count;
}

int
db_lrange(db_t *db, const char *key, size_t key_len,
          int start, int stop,
          const char **out_data, size_t *out_len, int out_cap)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_list_node_t *n;
	int count, idx, written = 0;

	if (!e || e->type != DB_VAL_LIST)
		return 0;
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
		return 0;

	count = (int)e->val.list.count;
	if (start < 0) start = count + start;
	if (stop < 0)  stop = count + stop;
	if (start < 0) start = 0;
	if (stop >= count) stop = count - 1;
	if (start > stop)
		return 0;

	idx = 0;
	for (n = e->val.list.head; n && written < out_cap; n = n->next) {
		if (idx >= start && idx <= stop) {
			out_data[written] = n->data;
			out_len[written] = n->len;
			written++;
		}
		idx++;
	}
	return written;
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
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
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
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
		return 0;
	return e->val.hash.count;
}

int
db_hkeys(db_t *db, const char *key, size_t key_len,
         const char **out_keys, size_t *out_lens, int out_cap)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_hash_field_t *f;
	int i = 0;

	if (!e || e->type != DB_VAL_HASH)
		return 0;
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
		return 0;

	for (f = e->val.hash.head; f && i < out_cap; f = f->next, i++) {
		out_keys[i] = f->data;
		out_lens[i] = f->key_len;
	}
	return i;
}

int
db_hvals(db_t *db, const char *key, size_t key_len,
         const char **out_vals, size_t *out_lens, int out_cap)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_hash_field_t *f;
	int i = 0;

	if (!e || e->type != DB_VAL_HASH)
		return 0;
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
		return 0;

	for (f = e->val.hash.head; f && i < out_cap; f = f->next, i++) {
		out_vals[i] = f->data + f->key_len + 1;
		out_lens[i] = f->val_len;
	}
	return i;
}

int
db_hgetall(db_t *db, const char *key, size_t key_len,
           const char **out_keys, size_t *out_key_lens,
           const char **out_vals, size_t *out_val_lens, int out_cap)
{
	const db_entry_t *e = db_lookup(db, key, key_len);
	const db_hash_field_t *f;
	int i = 0;

	if (!e || e->type != DB_VAL_HASH)
		return 0;
	if (e->expire_at_ns > 0 && __os_clock_mono() >= e->expire_at_ns)
		return 0;

	for (f = e->val.hash.head; f && i < out_cap; f = f->next, i++) {
		out_keys[i] = f->data;
		out_key_lens[i] = f->key_len;
		out_vals[i] = f->data + f->key_len + 1;
		out_val_lens[i] = f->val_len;
	}
	return i;
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

int
db_keys(db_t *db, const char *pattern, size_t pattern_len,
        const char **out_keys, size_t *out_lens, int out_cap)
{
	const db_table_t *tbl = xtc_lrlock_read_data(db->lr);
	int64_t now = __os_clock_mono();
	int count = 0;
	size_t i;

	for (i = 0; i < tbl->n_buckets && count < out_cap; i++) {
		const db_entry_t *e;
		for (e = tbl->buckets[i]; e && count < out_cap; e = e->next) {
			if (e->expire_at_ns > 0 && now >= e->expire_at_ns)
				continue;
			if (glob_match(pattern, pattern_len, e->key, e->key_len)) {
				out_keys[count] = e->key;
				out_lens[count] = e->key_len;
				count++;
			}
		}
	}
	return count;
}

size_t
db_key_count(db_t *db)
{
	const db_table_t *tbl;
	db_read_begin(db);
	tbl = xtc_lrlock_read_data(db->lr);
	db_read_end(db);
	return tbl->n_keys;
}

size_t
db_mem_used(db_t *db)
{
	const db_table_t *tbl;
	db_read_begin(db);
	tbl = xtc_lrlock_read_data(db->lr);
	db_read_end(db);
	return tbl->mem_used;
}

/* ----- Write operations ----- */

static db_table_t *
db_write_table(db_t *db)
{
	return (db_table_t *)xtc_lrlock_write_data(db->lr);
}

int
db_set(db_t *db, const char *key, size_t key_len,
       const char *val, size_t val_len)
{
	return db_set_ex(db, key, key_len, val, val_len, 0);
}

int
db_set_ex(db_t *db, const char *key, size_t key_len,
          const char *val, size_t val_len, int64_t expire_ns)
{
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
	db_entry_t *e;
	char *val_copy;
	size_t idx;

	if (key_len > DB_MAX_KEY_LEN)
		return -1;

	e = entry_find(tbl, key, key_len, h);
	if (e) {
		/* Replace existing */
		entry_free_value(db, tbl, e);
		val_copy = db_malloc(db, tbl, val_len + 1);
		if (!val_copy)
			return -1;
		memcpy(val_copy, val, val_len);
		val_copy[val_len] = '\0';
		e->type = DB_VAL_STRING;
		e->val.str.data = val_copy;
		e->val.str.len = val_len;
		e->expire_at_ns = expire_ns;
		return 0;
	}

	/* Check key limit */
	if (db->opts.max_keys > 0 && tbl->n_keys >= db->opts.max_keys)
		return -1;

	/* New entry */
	e = db_malloc(db, tbl, entry_size(key_len));
	if (!e)
		return -1;

	val_copy = db_malloc(db, tbl, val_len + 1);
	if (!val_copy) {
		db_free(db, tbl, e, entry_size(key_len));
		return -1;
	}

	memcpy(val_copy, val, val_len);
	val_copy[val_len] = '\0';
	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->hash = h;
	e->type = DB_VAL_STRING;
	e->expire_at_ns = expire_ns;
	e->val.str.data = val_copy;
	e->val.str.len = val_len;

	idx = h % tbl->n_buckets;
	e->next = tbl->buckets[idx];
	tbl->buckets[idx] = e;
	tbl->n_keys++;

	return 0;
}

int
db_del(db_t *db, const char *key, size_t key_len)
{
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
	db_entry_t *e = entry_find(tbl, key, key_len, h);

	if (!e)
		return 0;
	entry_remove(db, tbl, e);
	return 1;
}

int
db_expire(db_t *db, const char *key, size_t key_len, int64_t expire_ns)
{
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
	db_entry_t *e = entry_find(tbl, key, key_len, h);

	if (!e)
		return -1;
	e->expire_at_ns = expire_ns;
	return 0;
}

int64_t
db_incr(db_t *db, const char *key, size_t key_len, int64_t delta)
{
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
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
	db_free(db, tbl, e->val.str.data, e->val.str.len + 1);
	e->val.str.data = db_malloc(db, tbl, (size_t)n + 1);
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
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	size_t idx;

	if (e) {
		if (e->type != DB_VAL_LIST)
			return NULL;
		return e;
	}

	/* Check key limit */
	if (db->opts.max_keys > 0 && tbl->n_keys >= db->opts.max_keys)
		return NULL;

	e = db_malloc(db, tbl, entry_size(key_len));
	if (!e)
		return NULL;

	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->hash = h;
	e->type = DB_VAL_LIST;
	e->expire_at_ns = 0;
	e->val.list.head = NULL;
	e->val.list.tail = NULL;
	e->val.list.count = 0;

	idx = h % tbl->n_buckets;
	e->next = tbl->buckets[idx];
	tbl->buckets[idx] = e;
	tbl->n_keys++;

	return e;
}

int64_t
db_lpush(db_t *db, const char *key, size_t key_len,
         const char *val, size_t val_len)
{
	db_table_t *tbl = db_write_table(db);
	db_entry_t *e = db_get_or_create_list(db, key, key_len);
	db_list_node_t *n;

	if (!e)
		return -1;

	n = db_malloc(db, tbl, sizeof(*n) + val_len + 1);
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
	db_table_t *tbl = db_write_table(db);
	db_entry_t *e = db_get_or_create_list(db, key, key_len);
	db_list_node_t *n;

	if (!e)
		return -1;

	n = db_malloc(db, tbl, sizeof(*n) + val_len + 1);
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
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
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
	db_free(db, tbl, n, sizeof(*n) + n->len + 1);

	/* Remove empty list */
	if (e->val.list.count == 0)
		entry_remove(db, tbl, e);

	return 1;
}

int
db_rpop(db_t *db, const char *key, size_t key_len,
        db_pop_cb cb, void *user)
{
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
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
	db_free(db, tbl, n, sizeof(*n) + n->len + 1);

	/* Remove empty list */
	if (e->val.list.count == 0)
		entry_remove(db, tbl, e);

	return 1;
}

/* ----- Hash operations ----- */

static db_entry_t *
db_get_or_create_hash(db_t *db, const char *key, size_t key_len)
{
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	size_t idx;

	if (e) {
		if (e->type != DB_VAL_HASH)
			return NULL;
		return e;
	}

	/* Check key limit */
	if (db->opts.max_keys > 0 && tbl->n_keys >= db->opts.max_keys)
		return NULL;

	e = db_malloc(db, tbl, entry_size(key_len));
	if (!e)
		return NULL;

	memcpy(e->key, key, key_len);
	e->key[key_len] = '\0';
	e->key_len = key_len;
	e->hash = h;
	e->type = DB_VAL_HASH;
	e->expire_at_ns = 0;
	e->val.hash.head = NULL;
	e->val.hash.count = 0;

	idx = h % tbl->n_buckets;
	e->next = tbl->buckets[idx];
	tbl->buckets[idx] = e;
	tbl->n_keys++;

	return e;
}

int
db_hset(db_t *db, const char *key, size_t key_len,
        const char *field, size_t field_len,
        const char *val, size_t val_len)
{
	db_table_t *tbl = db_write_table(db);
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
			db_hash_field_t *nf = db_malloc(db, tbl,
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
			db_free(db, tbl, f,
			        sizeof(*f) + f->key_len + f->val_len + 2);
			return 0;
		}
	}

	/* New field */
	f = db_malloc(db, tbl, sizeof(*f) + field_len + val_len + 2);
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
	db_table_t *tbl = db_write_table(db);
	uint64_t h = fnv1a(key, key_len);
	db_entry_t *e = entry_find(tbl, key, key_len, h);
	db_hash_field_t **pp, *f;

	if (!e || e->type != DB_VAL_HASH)
		return 0;

	for (pp = &e->val.hash.head; *pp; pp = &(*pp)->next) {
		f = *pp;
		if (f->key_len == field_len &&
		    memcmp(f->data, field, field_len) == 0) {
			*pp = f->next;
			db_free(db, tbl, f,
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
	db_table_t *tbl = db_write_table(db);
	size_t i;
	db_entry_t *e, *e_next;

	for (i = 0; i < tbl->n_buckets; i++) {
		for (e = tbl->buckets[i]; e; e = e_next) {
			e_next = e->next;
			entry_free_value(db, tbl, e);
			db_free(db, tbl, e, entry_size(e->key_len));
		}
		tbl->buckets[i] = NULL;
	}
	tbl->n_keys = 0;
}

int
db_expire_stale(db_t *db, int64_t now_ns, int max_scan)
{
	db_table_t *tbl = db_write_table(db);
	int removed = 0, scanned = 0;
	size_t i;

	for (i = 0; i < tbl->n_buckets && scanned < max_scan; i++) {
		db_entry_t **pp = &tbl->buckets[i];
		while (*pp && scanned < max_scan) {
			db_entry_t *e = *pp;
			scanned++;
			if (e->expire_at_ns > 0 && now_ns >= e->expire_at_ns) {
				*pp = e->next;
				entry_free_value(db, tbl, e);
				db_free(db, tbl, e, entry_size(e->key_len));
				if (tbl->n_keys > 0)
					tbl->n_keys--;
				removed++;
			} else {
				pp = &e->next;
			}
		}
	}
	return removed;
}
