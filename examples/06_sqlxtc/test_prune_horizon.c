/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_prune_horizon.c
 *	A write computes the GC horizon only when it prunes.  Autovacuum
 *	prunes one write in up to 256 once prunes stop finding dead
 *	versions, but every written row computed the horizon first (a
 *	seq_cst fence and a load of each of the 256 snapshot pin slots),
 *	and the countdown then threw it away: 4.5% of a TPC-C connection's
 *	CPU at one connection.
 *	  N autocommit INSERTs of new rows, then one transaction of N
 *	  INSERTs (nothing to prune: the prunes back off): the horizon is
 *	  computed far fewer than N times for each (here: under N / 8).
 *	#includes xstore.c to count snap_horizon calls (XS_HORIZON_TEST_HOOK).
 */

#include <stdatomic.h>
#include <unistd.h>

static _Atomic long g_hz;
#define XS_HORIZON_TEST_HOOK() ((void)atomic_fetch_add(&g_hz, 1))
#include "xstore.c"

#include "engine.h"
#include "t_tmp.h"

#define N 2000

static int
ex(sx_db *h, const char *sql)
{
	char *err = NULL;
	int rc = sx_exec(h, sql, &err);
	free(err);
	return rc;
}

int
main(void)
{
	char path[256], wal[300], q[96];
	sx_db *h = NULL;
	long a0, a1, t1;
	int i, fd;

	t_tmpl(path, sizeof path, "sqlxtc-prune-hz");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 1024) == SX_OK);
	CK(sx_open(path, &h) == SX_OK);
	if (h == NULL) return 1;
	CK(ex(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)") == SX_OK);

	a0 = atomic_load(&g_hz);
	for (i = 1; i <= N; i++) {
		snprintf(q, sizeof q, "INSERT INTO t VALUES(%d, %d)", i, i);
		CK(ex(h, q) == SX_OK);
	}
	a1 = atomic_load(&g_hz) - a0;

	CK(ex(h, "BEGIN") == SX_OK);
	for (i = N + 1; i <= 2 * N; i++) {
		snprintf(q, sizeof q, "INSERT INTO t VALUES(%d, %d)", i, i);
		CK(ex(h, q) == SX_OK);
	}
	a0 = atomic_load(&g_hz);
	CK(ex(h, "COMMIT") == SX_OK);
	t1 = atomic_load(&g_hz) - a0;

	printf("  %d autocommit inserts: %ld horizon computations; a %d-row "
	    "commit: %ld\n", N, a1, N, t1);
	CK(a1 < N / 8);
	CK(t1 < N / 8);

	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	unlink(path); unlink(wal);
	if (g_fail) { fprintf(stderr, "FAIL: test_prune_horizon\n"); return 1; }
	printf("  ok   a write computes the GC horizon only when it prunes\n");
	return 0;
}
