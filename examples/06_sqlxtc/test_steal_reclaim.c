/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_steal_reclaim.c
 *	A transaction's spilled payloads (STEAL staging rows) must leave the
 *	tree when the transaction ends.  A transaction that buffers more than
 *	XS_SPILL_HI of row payloads spills the older ones into the tree under
 *	the staging table-id; nothing deleted them at COMMIT or ROLLBACK, so
 *	every bulk load left a second copy of its rows in the page file until
 *	the next crash rebuild.  Commits a big transaction and rolls back
 *	another, then counts staging keys (xstore_stage_rows): must be 0.
 *	Also: a storage-engine connection prunes old versions by default
 *	(autovacuum): 600 updates of one row run prune passes (the adaptive
 *	gate backs off to one pass per 256 writes after the inserts).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static int
load(sx_db *h, int base, int n, const char *end)
{
	sx_stmt *ins = NULL;
	int i;
	if (sx_exec(h, "BEGIN", NULL) != SX_OK) return -1;
	if (sx_prepare(h, "INSERT INTO t VALUES(?, ?)", -1, &ins, NULL) != SX_OK) return -1;
	for (i = 0; i < n; i++) {
		sx_bind_int64(ins, 1, base + i);
		sx_bind_text(ins, 2, "pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp", 80);
		if (sx_step(ins) != SX_DONE) { sx_finalize(ins); return -1; }
		sx_reset(ins);
	}
	sx_finalize(ins);
	return sx_exec(h, end, NULL) == SX_OK ? 0 : -1;
}

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -3;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

int
main(void)
{
	char path[256], side[320];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *h = NULL;
	int fd, i;
	uint64_t s1, s2;

	t_tmpl(path, sizeof path, "sqlxtc-stealrc");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 1024) == SX_OK);
	CK(sx_open(path, &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, pad TEXT)", NULL) == SX_OK);
	CK(load(h, 0, 20000, "COMMIT") == 0);
	s1 = xstore_stage_rows(xstore_bt_of((struct xsql *)h));
	CK(load(h, 100000, 20000, "ROLLBACK") == 0);
	s2 = xstore_stage_rows(xstore_bt_of((struct xsql *)h));
	printf("  staging rows left: after a 20000-row COMMIT %llu, after a 20000-row ROLLBACK %llu\n",
	    (unsigned long long)s1, (unsigned long long)s2);
	CK(s1 == 0);
	CK(s2 == 0);
	CK(one(h, "SELECT count(*) FROM t") == 20000);
	/* autovacuum is on by default for a storage-engine connection */
	{
		int64_t p0 = xstore_prune_count();
		for (i = 0; i < 600; i++)
			CK(sx_exec(h, "UPDATE t SET pad = 'x' WHERE k = 7", NULL) == SX_OK);
		printf("  prune passes over 600 updates of one row: %lld\n",
		    (long long)(xstore_prune_count() - p0));
		CK(xstore_prune_count() - p0 > 0);
	}
	/* and a crash afterwards still recovers exactly the committed rows */
	sx_close(h);
	sx_storage_abandon();
	CK(sx_storage_open(path, 1024) == SX_OK);
	CK(sx_open(path, &h) == SX_OK);
	CK(one(h, "SELECT count(*) FROM t") == 20000);
	sx_close(h);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) { snprintf(side, sizeof side, "%s%s", path, sfx[i]); unlink(side); }
	if (g_fail) { fprintf(stderr, "FAIL: test_steal_reclaim\n"); return 1; }
	printf("  ok   spilled payloads leave the tree when their transaction ends\n");
	return 0;
}
