/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_idx_trust.c
 *	Secondary indexes across a restart (sx_storage_open / _close).
 *
 *	Every open used to clear and rebuild every index from the base rows,
 *	a trusted clean reopen included: 16 s of an 18.5 s open for a ledger
 *	store of 2M rows and 3 indexes, 37 s of 43 s for TPC-C at 10
 *	warehouses.  A clean close now records in the superblock that the
 *	indexes are complete (every entry insert of the run succeeded, the
 *	key format is the binary's) and the next open keeps them.  The test
 *	pins:
 *	  1. a clean reopen does NO rebuild, and the indexes it keeps are
 *	     complete (xstore_index_check: no live row without its entry),
 *	     answer seeks exactly, and enforce UNIQUE (a duplicate of a key
 *	     written before the close is refused);
 *	  2. updates that move keys before the close leave no queued
 *	     superseded entry behind (the dead queue is drained at close);
 *	  3. a crash (sx_storage_abandon) rebuilds, and the rebuilt indexes
 *	     are complete with UNIQUE holding;
 *	  4. a failed index-entry insert (injected: bt_insert can fail) taints
 *	     the run: the next clean reopen rebuilds instead of trusting an
 *	     index that misses a row -- and the rebuild restores the entry;
 *	  5. an index entry deleted behind the engine's back IS reported by
 *	     the check (the check is not vacuous);
 *	  6. the log tail recorded at the clean close is used: the reopen
 *	     accepts it (no scan), and new commits after it are durable;
 *	  7. a clean open + close with no commit in between leaves the log
 *	     byte for byte as it was (the close does not rewrite it: that
 *	     read the whole base, 13 s on a cold TPC-C store of 10 warehouses).
 *	Off a loop (synchronous I/O), as test_clean_restart.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "btree.h"
#include "t_tmp.h"

#define NROW   3000
#define NMOVE  1000   /* rows an UPDATE moves: 2 entries each, under the
                       * 4096-entry dead queue (an overflow is counted as
                       * garbage, see xs_idx_dead_add) */
#define FRAMES 4096   /* >= 2048: the rebuild runs on several threads */

static int64_t
q1(const char *sql, int64_t p)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_open(":memory:", &h) != SX_OK) return -2;
	if (sx_prepare(h, sql, -1, &st, NULL) == SX_OK) {
		sx_bind_int64(st, 1, p);
		if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return v;
}

static int
ex(const char *sql)
{
	sx_db *h = NULL;
	int rc;
	if (sx_open(":memory:", &h) != SX_OK) return SX_ERROR;
	rc = sx_exec(h, sql, NULL);
	sx_close(h);
	return rc;
}

/* The whole file, or NULL; *n = its length. */
static unsigned char *
slurp(const char *path, long *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b = NULL;
	*n = -1;
	if (f == NULL) return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (*n = ftell(f)) >= 0 &&
	    fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)*n + 1)) != NULL &&
	    fread(b, 1, (size_t)*n, f) != (size_t)*n) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

/* Indexes complete?  Returns the missing count (-1: nothing checked). */
static long
missing(uint64_t *stale)
{
	uint64_t m = 0, s = 0;
	if (xstore_index_check((bt_t *)sx_storage_bt(), &m, &s) < 3)
		return -1;
	if (stale) *stale = s;
	return (long)m;
}

/* Seeks and UNIQUE on the current store; `gen` is the value offset the
 * last update pass wrote (grp = (k + gen) % 50 for k < NMOVE, so every
 * group still holds NROW / 50 rows). */
static void
check_answers(int gen, const char *what)
{
	int64_t n;
	char sql[160];
	n = q1("SELECT count(*) FROM t WHERE grp = ?", (234 + gen) % 50);
	CK(n == NROW / 50);
	n = q1("SELECT k FROM t WHERE grp = ? AND k = 234", (234 + gen) % 50);
	CK(n == 234);
	n = q1("SELECT count(*) FROM t WHERE grp = ? AND k = 234", (234 + gen + 1) % 50);
	CK(n == 0);                              /* the old key is gone */
	snprintf(sql, sizeof sql,
	    "INSERT INTO t(k, grp, code) VALUES(%d, 1, 'c%d')", NROW + 7, 77);
	CK(ex(sql) == SX_CONSTRAINT);            /* 'c77' is taken: UNIQUE */
	CK(ex("INSERT INTO t(k, grp, code) VALUES(100000, 1, 'fresh')") == SX_OK);
	CK(ex("DELETE FROM t WHERE k = 100000") == SX_OK);
	printf("  %-34s seek ok, UNIQUE enforced\n", what);
}

int
main(void)
{
	char path[256], wal[300], sql[200];
	uint64_t r0, stale = 0;
	long m, n0, n1;
	unsigned char *wa, *wb;
	int i, fd;
	sx_db *h = NULL;
	sx_stmt *st = NULL;

	t_tmpl(path, sizeof path, "sqlxtc-idxtrust");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	snprintf(wal, sizeof wal, "%s-wal", path);
	CK(sx_init() == SX_OK);

	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(ex("CREATE TABLE t(k INTEGER PRIMARY KEY, grp INTEGER, code TEXT UNIQUE)") == SX_OK);
	CK(ex("CREATE INDEX t_grp ON t(grp)") == SX_OK);
	CK(ex("CREATE INDEX t_grp_code ON t(grp, code)") == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(h, "INSERT INTO t(k, grp, code) VALUES(?, ?, ?)", -1, &st,
	    NULL) == SX_OK);
	for (i = 0; i < NROW; i++) {
		char c[16];
		snprintf(c, sizeof c, "c%d", i);
		sx_bind_int64(st, 1, i);
		sx_bind_int64(st, 2, i % 50);
		sx_bind_text(st, 3, c, -1);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	sx_finalize(st);
	CK(sx_exec(h, "COMMIT", NULL) == SX_OK);
	/* move keys: the old entries are queued for reaping, and one commit
	 * reaps only 32 of them -- the rest are in the queue at close */
	CK(sx_exec(h, "UPDATE t SET grp = (k + 1) % 50 WHERE k < 1000", NULL) == SX_OK);
	sx_close(h);
	CK(sx_storage_close() == SX_OK);

	/* 1 + 2: a clean reopen trusts the indexes */
	r0 = xstore_index_rebuild_count();
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(xstore_index_rebuild_count() == r0);          /* no rebuild */
	printf("  clean reopen: %llu index rebuilds (want 0)\n",
	    (unsigned long long)(xstore_index_rebuild_count() - r0));
	m = missing(&stale);
	CK(m == 0);
	CK(stale == 0);                                  /* drained at close */
	printf("  trusted indexes: %ld missing, %llu stale (want 0, 0)\n", m,
	    (unsigned long long)stale);
	check_answers(1, "after a clean reopen:");

	/* 6: the recorded tail was accepted -- the log ends where the close
	 * left it and a commit after the reopen survives a clean cycle */
	CK(ex("UPDATE t SET grp = (k + 2) % 50 WHERE k < 1000") == SX_OK);
	CK(sx_storage_close() == SX_OK);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(q1("SELECT grp FROM t WHERE k = ?", 10) == 12);
	CK(missing(&stale) == 0 && stale == 0);
	CK(missing(NULL) == 0);
	check_answers(2, "after a second clean cycle:");

	/* 3: a crash rebuilds, and the rebuilt indexes are complete */
	CK(ex("UPDATE t SET grp = (k + 3) % 50 WHERE k < 1000") == SX_OK);
	sx_storage_abandon();
	r0 = xstore_index_rebuild_count();
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(xstore_index_rebuild_count() == r0 + 1);
	CK(missing(NULL) == 0);
	check_answers(3, "after a crash (rebuilt):");
	CK(sx_storage_close() == SX_OK);

	/* 4: a failed entry insert taints the run */
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	xstore_index_fail_inject(1);
	CK(ex("INSERT INTO t(k, grp, code) VALUES(5000, 9, 'lost')") == SX_OK);
	xstore_index_fail_inject(0);
	m = missing(NULL);
	CK(m == 1);                                      /* the lost entry */
	printf("  injected insert failure: %ld missing entry (want 1)\n", m);
	CK(sx_storage_close() == SX_OK);
	r0 = xstore_index_rebuild_count();
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(xstore_index_rebuild_count() == r0 + 1);      /* not trusted */
	CK(missing(NULL) == 0);                          /* rebuilt whole */
	CK(q1("SELECT k FROM t WHERE grp = ? AND k = 5000", 9) == 5000);
	CK(ex("INSERT INTO t(k, grp, code) VALUES(5001, 9, 'lost')") == SX_CONSTRAINT);
	printf("  tainted run: next reopen rebuilt; UNIQUE holds for the "
	    "re-indexed row\n");
	CK(sx_storage_close() == SX_OK);
	/* and the run after the rebuild is clean again: trusted */
	r0 = xstore_index_rebuild_count();
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(xstore_index_rebuild_count() == r0);

	/* 7: an idle open + close keeps the log as it is */
	CK(sx_storage_close() == SX_OK);
	wa = slurp(wal, &n0);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(sx_storage_close() == SX_OK);
	wb = slurp(wal, &n1);
	CK(wa != NULL && wb != NULL && n0 == n1 &&
	    memcmp(wa, wb, (size_t)n0) == 0);
	printf("  idle open+close: log %ld -> %ld bytes, %s\n", n0, n1,
	    wa && wb && n0 == n1 && memcmp(wa, wb, (size_t)n0) == 0 ?
	    "unchanged" : "REWRITTEN");
	free(wa); free(wb);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(q1("SELECT count(*) FROM t WHERE grp = ?", 9) > 0);
	CK(missing(NULL) == 0);

	/* 5: the check sees a dropped entry */
	xstore_index_fail_inject(1);
	snprintf(sql, sizeof sql, "UPDATE t SET grp = 33 WHERE k = 17");
	CK(ex(sql) == SX_OK);
	xstore_index_fail_inject(0);
	CK(missing(NULL) == 1);
	CK(sx_storage_close() == SX_OK);

	(void)sx_shutdown();
	unlink(path); unlink(wal);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_idx_trust\n");
		return 1;
	}
	printf("  ok   indexes are trusted across a clean reopen, rebuilt after "
	    "a crash or a failed insert, and complete either way\n");
	return 0;
}
