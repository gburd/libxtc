/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test_pcache_xtc.c
 *	In-process test of the xtc_slab-backed SQLite page cache.
 *	Installs the pcache before any handle, runs a workload large
 *	enough to allocate many pages on an in-memory database (whose
 *	storage *is* the page cache), and asserts the slab actually
 *	served the pages and lookups hit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlite/sqlite3.h"
#include "pcache_xtc.h"

static int g_rows;

static int
count_cb(void *u, int n, char **v, char **names)
{
	(void)u; (void)names;
	if (n >= 1 && v[0] != NULL)
		g_rows = atoi(v[0]);
	return 0;
}

/* Bounded-memory test: a file-backed database with a small page cache
 * scanned over a working set far larger than the cache.  SQLite
 * unpins pages as the scan advances, so the slab pcache recycles them
 * -- live pages stay near the cache size instead of growing to the
 * whole database.  Returns 0 on pass. */
static int
eviction_test(void)
{
	sqlite3 *db = NULL;
	char *err = NULL;
	char path[] = "/tmp/sqlxtc-pcache-evict-XXXXXX";
	int fd, i, fails = 0;
	xtc_pcache_stats_t a, b;
	uint64_t peak_live;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd); unlink(path);

	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL(evict): open: %s\n", sqlite3_errmsg(db));
		unlink(path); return 1;
	}
	(void)sqlite3_exec(db, "PRAGMA journal_mode=DELETE;", 0, 0, 0);
	(void)sqlite3_exec(db,
	    "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);", 0, 0, 0);

	(void)sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	for (i = 1; i <= 8000; i++) {
		char sql[160];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(a,b) VALUES(%d,"
		    "'padding-padding-padding-padding-padding-%d');", i, i);
		if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
			fprintf(stderr, "FAIL(evict): insert: %s\n", err);
			sqlite3_free(err); sqlite3_close(db); unlink(path);
			return 1;
		}
	}
	(void)sqlite3_exec(db, "COMMIT;", 0, 0, 0);
	sqlite3_close(db);

	/* Reopen with a cold cache and a small cache_size (50 pages), then
	 * scan every leaf page.  The working set far exceeds the cache,
	 * so the slab pcache must recycle unpinned pages and keep its
	 * resident set bounded. */
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		unlink(path); return 1;
	}
	(void)sqlite3_exec(db, "PRAGMA cache_size=50;", 0, 0, 0);

	xtc_pcache_get_stats(&a);
	g_rows = -1;
	/* sum(length(b)) forces reading every row's payload -> every
	 * leaf page, defeating any count(*) shortcut. */
	(void)sqlite3_exec(db, "SELECT count(*) FROM "
	    "(SELECT a FROM t WHERE length(b) > 0);", count_cb, 0, 0);
	xtc_pcache_get_stats(&b);
	peak_live = b.live_pages;

	sqlite3_close(db);
	unlink(path);

	if (g_rows != 8000) {
		fprintf(stderr, "FAIL(evict): count=%d expected 8000\n", g_rows);
		fails++;
	}
	/* The recycle path must have fired (working set > cache), and the
	 * resident set must have stayed bounded well below the whole DB. */
	if (b.recycle - a.recycle == 0) {
		fprintf(stderr, "FAIL(evict): no recycling under pressure "
		    "(cache should have evicted)\n");
		fails++;
	} else if (peak_live > 200) {
		fprintf(stderr, "FAIL(evict): resident set %llu pages not "
		    "bounded (cache_size=50)\n",
		    (unsigned long long)peak_live);
		fails++;
	} else {
		printf("  ok   slab pcache bounds memory under pressure "
		       "(%llu recycles, resident <= %llu pages)\n",
		       (unsigned long long)(b.recycle - a.recycle),
		       (unsigned long long)peak_live);
	}
	return fails;
}

int
main(void)
{
	sqlite3 *db = NULL;
	char *err = NULL;
	int rc, i, fails = 0;
	xtc_pcache_stats_t s0, s1;

	/* Must install the pcache before the first handle / init. */
	if (xtc_pcache_register() != SQLITE_OK) {
		fprintf(stderr, "FAIL: xtc_pcache_register\n");
		return 2;
	}

	xtc_pcache_get_stats(&s0);

	rc = sqlite3_open(":memory:", &db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL: open :memory:: %s\n",
		    sqlite3_errmsg(db));
		return 2;
	}

	rc = sqlite3_exec(db,
	    "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);", 0, 0, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL: create: %s\n", err);
		sqlite3_free(err);
		return 2;
	}

	/* Enough rows to span many pages (each ~100 B value). */
	(void)sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	for (i = 1; i <= 5000; i++) {
		char sql[160];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(a,b) VALUES(%d,"
		    "'payload-padding-padding-padding-padding-%d');", i, i);
		rc = sqlite3_exec(db, sql, 0, 0, &err);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "FAIL: insert %d: %s\n", i, err);
			sqlite3_free(err);
			return 2;
		}
	}
	(void)sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	/* Repeated scans should hit resident pages. */
	for (i = 0; i < 5; i++) {
		g_rows = -1;
		rc = sqlite3_exec(db, "SELECT count(*) FROM t;",
		    count_cb, 0, &err);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "FAIL: count: %s\n", err);
			sqlite3_free(err);
			return 2;
		}
	}

	sqlite3_close(db);
	xtc_pcache_get_stats(&s1);

	if (g_rows != 5000) {
		fprintf(stderr, "FAIL: count(*)=%d expected 5000\n", g_rows);
		fails++;
	} else {
		printf("  ok   workload round-trips via slab pcache (%d rows)\n",
		    g_rows);
	}

	if (s1.slab_alloc <= s0.slab_alloc) {
		fprintf(stderr, "FAIL: no slab page allocations recorded\n");
		fails++;
	} else {
		printf("  ok   slab served pages (%llu allocations)\n",
		    (unsigned long long)(s1.slab_alloc - s0.slab_alloc));
	}

	if (s1.fetch_hit <= s0.fetch_hit) {
		fprintf(stderr, "FAIL: no page-cache hits recorded\n");
		fails++;
	} else {
		printf("  ok   page lookups hit cache (%llu hits, %llu miss)\n",
		    (unsigned long long)(s1.fetch_hit - s0.fetch_hit),
		    (unsigned long long)(s1.fetch_miss - s0.fetch_miss));
	}

	/* After close, every page this test created must be reclaimed. */
	if (s1.live_pages != s0.live_pages) {
		fprintf(stderr, "FAIL: %llu pages leaked (was %llu)\n",
		    (unsigned long long)s1.live_pages,
		    (unsigned long long)s0.live_pages);
		fails++;
	} else {
		printf("  ok   all pages reclaimed on close (live=%llu)\n",
		    (unsigned long long)s1.live_pages);
	}

	/* Bounded memory under a working set larger than the cache. */
	fails += eviction_test();

	printf("%s\n", fails == 0 ? "All xtc pcache tests passed."
	                          : "xtc pcache test FAILED.");
	return fails == 0 ? 0 : 1;
}
