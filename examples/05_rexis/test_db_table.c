/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/test_db_table.c
 *	Keyspace table behaviour at scale: the table grows with the key
 *	count (lookups stay O(1) instead of walking ever-longer chains),
 *	every key stays reachable across growth, growth is charged to the
 *	memory budget, and active expiry visits the whole table.  A shared
 *	(multi-loop) keyspace is sharded: writers to keys in different
 *	shards run at once, writers to one shard exclude each other, and
 *	the keyspace-wide key limit still holds across shards.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "t_assert.h"

#define N_BIG   400000

static int
mk(char *buf, size_t cap, const char *pfx, int i)
{
	return snprintf(buf, cap, "%s%d", pfx, i);
}

/*
 * Lookups must not slow down as the table fills.  Insert N_BIG keys into
 * a table that starts at 1,024 buckets, then time a fixed batch of GETs
 * at 4,000 keys and at N_BIG keys.  A fixed-size chained table walks
 * ~N/buckets entries per lookup, so the second batch is ~100x slower; a
 * table that grows keeps it flat.  The bound (8x) leaves room for cache
 * misses on a 100x larger working set.  Also checks every key is still
 * found after all the growth (no entry lost in a rehash).
 */
static void
test_grows_and_stays_fast(void)
{
	db_t *db = NULL;
	db_opts_t o = DB_OPTS_DEFAULT;
	char k[32];
	const char *v;
	size_t vl;
	int i, n, r;
	int64_t t0, small_ns, big_ns;

	o.n_buckets = 1024;
	ASSERT(db_create(&o, &db) == 0);

	db_write_begin(db);
	for (i = 0; i < 4000; i++) {
		n = mk(k, sizeof k, "key:", i);
		ASSERT(db_set(db, k, (size_t)n, k, (size_t)n) == 0);
	}
	db_write_end(db);

	db_read_begin(db);
	t0 = xtc_clock_mono();
	for (r = 0; r < 50; r++)
		for (i = 0; i < 4000; i++) {
			n = mk(k, sizeof k, "key:", i);
			ASSERT(db_get(db, k, (size_t)n, &v, &vl) == 0);
		}
	small_ns = xtc_clock_mono() - t0;
	db_read_end(db);

	db_write_begin(db);
	for (i = 4000; i < N_BIG; i++) {
		n = mk(k, sizeof k, "key:", i);
		ASSERT(db_set(db, k, (size_t)n, k, (size_t)n) == 0);
	}
	db_write_end(db);
	ASSERT(db_key_count(db) == N_BIG);

	db_read_begin(db);
	t0 = xtc_clock_mono();
	for (r = 0; r < 50; r++)
		for (i = 0; i < 4000; i++) {
			n = mk(k, sizeof k, "key:",
			    (int)(((long)i * 97) % N_BIG));
			ASSERT(db_get(db, k, (size_t)n, &v, &vl) == 0);
		}
	big_ns = xtc_clock_mono() - t0;
	for (i = 0; i < N_BIG; i++) {
		n = mk(k, sizeof k, "key:", i);
		ASSERT(db_get(db, k, (size_t)n, &v, &vl) == 0);
		ASSERT(vl == (size_t)n && memcmp(v, k, vl) == 0);
	}
	db_read_end(db);

	printf("  200k GETs: %.1f ms at 4k keys, %.1f ms at %dk keys (%.1fx)\n",
	    (double)small_ns / 1e6, (double)big_ns / 1e6, N_BIG / 1000,
	    (double)big_ns / (double)(small_ns > 0 ? small_ns : 1));
	ASSERT(big_ns < 8 * small_ns);

	/* Deletes and re-inserts keep working on the grown table. */
	db_write_begin(db);
	for (i = 0; i < N_BIG; i += 2) {
		n = mk(k, sizeof k, "key:", i);
		ASSERT(db_del(db, k, (size_t)n) == 1);
	}
	db_write_end(db);
	ASSERT(db_key_count(db) == N_BIG / 2);
	db_read_begin(db);
	for (i = 0; i < N_BIG; i++) {
		n = mk(k, sizeof k, "key:", i);
		ASSERT((db_get(db, k, (size_t)n, &v, &vl) == 0) ==
		    (i % 2 == 1));
	}
	db_read_end(db);
	db_destroy(db);
	printf("  ok   grows_and_stays_fast\n");
}

/*
 * Growing the table is a keyspace allocation like any other: it is
 * charged to the memory budget (and released on destroy), and when the
 * budget refuses it the table keeps working at its current size -- a
 * write is never failed just because the table could not grow.
 */
static void
test_growth_is_budgeted(void)
{
	xtc_res_t res;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	db_t *db = NULL;
	db_opts_t o = DB_OPTS_DEFAULT;
	char k[32];
	const char *v;
	size_t vl, empty, per_key;
	int i, n;

	caps.mem_bytes = 0;
	ASSERT(xtc_res_init(&res, &caps) == XTC_OK);
	o.n_buckets = 1024;
	o.res = &res;
	ASSERT(db_create(&o, &db) == 0);
	empty = db_mem_used(db);
	ASSERT((size_t)xtc_res_used(&res, XTC_RES_MEM_BYTES) == empty);
	db_write_begin(db);
	for (i = 0; i < 20000; i++) {
		n = mk(k, sizeof k, "b:", i);
		ASSERT(db_set(db, k, (size_t)n, "v", 1) == 0);
	}
	db_flushdb(db);
	db_write_end(db);
	/* The entries are gone; what is left is the grown table, still
	 * charged (it does not shrink). */
	ASSERT(db_key_count(db) == 0);
	ASSERT((size_t)xtc_res_used(&res, XTC_RES_MEM_BYTES) ==
	    db_mem_used(db));
	printf("  table: %zu bytes empty, %zu after 20k keys + FLUSHDB\n",
	    empty, db_mem_used(db));
	ASSERT(db_mem_used(db) > empty);
	db_destroy(db);
	ASSERT(xtc_res_used(&res, XTC_RES_MEM_BYTES) == 0);

	/* Fixed-width keys so every key costs the same.  Measure it at a
	 * size that needs no growth, then cap the keyspace at 1,100 keys:
	 * the split due at key 1,025 needs a new 8 KiB bucket segment the
	 * remaining 75 keys' room cannot cover, so it is refused -- and
	 * all 1,100 SETs must still succeed, with no memory taken by the
	 * table.  Key 1,101 is a real -OOM. */
	ASSERT(xtc_res_init(&res, &caps) == XTC_OK);
	ASSERT(db_create(&o, &db) == 0);
	db_write_begin(db);
	for (i = 0; i < 1000; i++) {
		n = snprintf(k, sizeof k, "c:%06d", i);
		ASSERT(db_set(db, k, (size_t)n, "v", 1) == 0);
	}
	db_write_end(db);
	per_key = (db_mem_used(db) - empty) / 1000;
	ASSERT(per_key * 1000 == db_mem_used(db) - empty);
	ASSERT(75 * per_key < LH_SEG * sizeof(void *));
	db_destroy(db);
	o.max_mem_bytes = (int64_t)(empty + 1100 * per_key);
	ASSERT(db_create(&o, &db) == 0);
	db_write_begin(db);
	for (i = 0; i < 1100; i++) {
		n = snprintf(k, sizeof k, "c:%06d", i);
		ASSERT(db_set(db, k, (size_t)n, "v", 1) == 0);
	}
	n = snprintf(k, sizeof k, "c:%06d", 1100);
	ASSERT(db_set(db, k, (size_t)n, "v", 1) == -1);
	db_write_end(db);
	ASSERT(db_key_count(db) == 1100);
	ASSERT(db_mem_used(db) == empty + 1100 * per_key);
	db_read_begin(db);
	for (i = 0; i < 1100; i++) {
		n = snprintf(k, sizeof k, "c:%06d", i);
		ASSERT(db_get(db, k, (size_t)n, &v, &vl) == 0);
	}
	db_read_end(db);
	db_destroy(db);
	ASSERT(xtc_res_used(&res, XTC_RES_MEM_BYTES) == 0);
	printf("  ok   growth_is_budgeted\n");
}

/*
 * Active expiry must make progress over the WHOLE table.  It used to
 * restart at bucket 0 on every pass and stop after max_scan entries, so
 * with live keys in the first buckets the expired keys behind them were
 * never reclaimed.
 */
static void
test_expiry_covers_table(void)
{
	db_t *db = NULL;
	db_opts_t o = DB_OPTS_DEFAULT;
	char k[32];
	int i, n, tick, removed = 0;
	int64_t later;

	ASSERT(db_create(&o, &db) == 0);
	db_write_begin(db);
	for (i = 0; i < 1000; i++) {
		n = mk(k, sizeof k, "live", i);
		ASSERT(db_set(db, k, (size_t)n, "v", 1) == 0);
	}
	for (i = 0; i < 20000; i++) {
		n = mk(k, sizeof k, "ttl", i);
		ASSERT(db_set_ex(db, k, (size_t)n, "v", 1,
		    xtc_clock_mono() + 1) == 0);
	}
	db_write_end(db);
	later = xtc_clock_mono() + 1000000000LL;
	/* 100 entries per pass, as expire.c: enough passes to sweep the
	 * table a few times over. */
	for (tick = 0; tick < 2000 && db_key_count(db) > 1000; tick++) {
		db_write_begin(db);
		removed += db_expire_stale(db, later, 100);
		db_write_end(db);
	}
	printf("  reclaimed %d of 20000 expired keys in %d passes\n",
	    removed, tick);
	ASSERT(removed == 20000);
	ASSERT(db_key_count(db) == 1000);
	db_destroy(db);
	printf("  ok   expiry_covers_table\n");
}

/* Two OS threads, each holding the write section of one key's shard.
 * Keys in different shards must be held at the same time (sharded
 * latches); keys in the same shard must not (exclusion).  The holder
 * spins until the other thread is inside too, bounded; with one latch
 * for the whole keyspace (1.59) the different-shard case never
 * overlaps.  Then the keyspace-wide --max-keys limit across shards. */
struct sh_arg {
	db_t           *db;
	const char     *key;
	_Atomic int    *inside;
	_Atomic int    *maxin;
};

static void *
sh_writer(void *arg)
{
	struct sh_arg *a = arg;
	db_keyset_t ks = db_key_shard(a->db, a->key, strlen(a->key));
	int64_t end;
	int n;

	db_lock(a->db, ks, 1);
	n = atomic_fetch_add(a->inside, 1) + 1;
	if (n > atomic_load(a->maxin))
		atomic_store(a->maxin, n);
	end = xtc_clock_mono() + 200LL * 1000 * 1000;
	while (atomic_load(a->inside) < 2 && xtc_clock_mono() < end)
		;                       /* wait for the other writer, bounded */
	(void)db_set(a->db, a->key, strlen(a->key), "v", 1);
	atomic_fetch_sub(a->inside, 1);
	db_unlock(a->db, ks, 1);
	return NULL;
}

static int
sh_overlap(db_t *db, const char *k1, const char *k2)
{
	_Atomic int inside = 0, maxin = 0;
	struct sh_arg a = { db, k1, &inside, &maxin };
	struct sh_arg b = { db, k2, &inside, &maxin };
	pthread_t t1, t2;

	ASSERT(pthread_create(&t1, NULL, sh_writer, &a) == 0);
	ASSERT(pthread_create(&t2, NULL, sh_writer, &b) == 0);
	(void)pthread_join(t1, NULL);
	(void)pthread_join(t2, NULL);
	return atomic_load(&maxin);
}

static void
test_shards_write_in_parallel(void)
{
	db_t *db = NULL;
	db_opts_t o = DB_OPTS_DEFAULT;
	char k1[32], k2[32];
	int i, n, ok = 0, refused = 0;

	o.shared = 1;
	o.max_keys = 100;
	ASSERT(db_create(&o, &db) == 0);
	/* Two keys in different shards, and one more in k1's shard. */
	(void)mk(k1, sizeof k1, "a", 0);
	for (i = 1; i < 1000; i++) {
		(void)mk(k2, sizeof k2, "a", i);
		if (db_key_shard(db, k2, strlen(k2)) !=
		    db_key_shard(db, k1, strlen(k1)))
			break;
	}
	ASSERT(db_key_shard(db, k1, strlen(k1)) !=
	    db_key_shard(db, k2, strlen(k2)));
	n = sh_overlap(db, k1, k2);
	printf("  different shards: %d writer(s) inside at once\n", n);
	ASSERT(n == 2);
	for (i = 1; i < 100000; i++) {
		(void)mk(k2, sizeof k2, "b", i);
		if (db_key_shard(db, k2, strlen(k2)) ==
		    db_key_shard(db, k1, strlen(k1)))
			break;
	}
	ASSERT(db_key_shard(db, k1, strlen(k1)) ==
	    db_key_shard(db, k2, strlen(k2)));
	n = sh_overlap(db, k1, k2);
	printf("  same shard: %d writer(s) inside at once\n", n);
	ASSERT(n == 1);
	/* The key limit is keyspace-wide, not per shard. */
	db_write_begin(db);
	db_flushdb(db);
	db_write_end(db);
	for (i = 0; i < 150; i++) {
		db_keyset_t ks;
		n = mk(k2, sizeof k2, "lim", i);
		ks = db_key_shard(db, k2, (size_t)n);
		db_lock(db, ks, 1);
		if (db_set(db, k2, (size_t)n, "v", 1) == 0)
			ok++;
		else
			refused++;
		db_unlock(db, ks, 1);
	}
	printf("  key limit 100 over shards: %d set, %d refused, %zu keys\n",
	    ok, refused, db_key_count(db));
	ASSERT(ok == 100 && refused == 50 && db_key_count(db) == 100);
	db_destroy(db);
	printf("  ok   shards_write_in_parallel\n");
}

int
main(void)
{
	test_grows_and_stays_fast();
	test_growth_is_budgeted();
	test_expiry_covers_table();
	test_shards_write_in_parallel();
	printf("All db table tests passed.\n");
	return 0;
}
