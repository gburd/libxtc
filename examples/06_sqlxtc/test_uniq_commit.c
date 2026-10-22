/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_uniq_commit.c
 *	The commit-time UNIQUE re-validation may skip a key only when no
 *	commit carrying it can have happened since the transaction's
 *	statement-time check.  Two connections, deterministic order, no
 *	threads:
 *
 *	(a) A: BEGIN; INSERT (id 1, key 'x') -- the statement check passes.
 *	    B: autocommit INSERT (id 2, key 'x') -- commits.
 *	    A: COMMIT must fail "UNIQUE constraint failed".
 *	(b) Same with B committing in a transaction.
 *	(c) Many unrelated commits between A's check and A's COMMIT (other
 *	    keys, same table): A's COMMIT must succeed (the skip may apply).
 *	(d) A unique index created between A's statement and A's COMMIT
 *	    while B commits a duplicate under it: A's COMMIT must fail.
 *	Each case is checked after the fact too: never two live rows with
 *	one key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

static int g_fail;

#undef CK
#define CK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
	__FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
is_uniq(sx_db *h, int rc)
{
	const char *m = sx_errmsg(h);
	return rc != SX_OK && m != NULL && strstr(m, "UNIQUE constraint failed") != NULL;
}

int
main(void)
{
	char path[256], side[320], q[128];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *a = NULL, *b = NULL;
	int fd, i, rc;

	t_tmpl(path, sizeof path, "sqlxtc-uniqcommit");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	if (sx_init() != SX_OK || sx_storage_open(path, 1024) != SX_OK ||
	    sx_open(path, &a) != SX_OK || sx_open(path, &b) != SX_OK)
		return 1;
	CK(sx_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, k TEXT, n INTEGER)", NULL) == SX_OK);
	CK(sx_exec(a, "CREATE UNIQUE INDEX t_k ON t(k)", NULL) == SX_OK);
	CK(sx_exec(a, "CREATE TABLE u(id INTEGER PRIMARY KEY, k TEXT)", NULL) == SX_OK);

	/* (a) */
	CK(sx_exec(a, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(a, "INSERT INTO t VALUES(1, 'x', 0)", NULL) == SX_OK);
	CK(sx_exec(b, "INSERT INTO t VALUES(2, 'x', 0)", NULL) == SX_OK);
	rc = sx_exec(a, "COMMIT", NULL);
	CK(is_uniq(a, rc));
	CK(one(a, "SELECT count(*) FROM t WHERE k = 'x'") == 1);

	/* (b) */
	CK(sx_exec(a, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(a, "INSERT INTO t VALUES(3, 'y', 0)", NULL) == SX_OK);
	CK(sx_exec(b, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(b, "INSERT INTO t VALUES(4, 'y', 0)", NULL) == SX_OK);
	CK(sx_exec(b, "COMMIT", NULL) == SX_OK);
	rc = sx_exec(a, "COMMIT", NULL);
	CK(is_uniq(a, rc));
	CK(one(a, "SELECT count(*) FROM t WHERE k = 'y'") == 1);

	/* (c) */
	CK(sx_exec(a, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(a, "INSERT INTO t VALUES(5, 'z', 0)", NULL) == SX_OK);
	for (i = 0; i < 200; i++) {
		snprintf(q, sizeof q, "INSERT INTO t VALUES(%d, 'o%d', 0)", 100 + i, i);
		CK(sx_exec(b, q, NULL) == SX_OK);
	}
	CK(sx_exec(a, "COMMIT", NULL) == SX_OK);
	CK(one(a, "SELECT count(*) FROM t WHERE k = 'z'") == 1);

	/* (d) a unique index created after A's statement check */
	CK(sx_exec(a, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(a, "INSERT INTO u VALUES(1, 'w')", NULL) == SX_OK);
	CK(sx_exec(b, "CREATE UNIQUE INDEX u_k ON u(k)", NULL) == SX_OK);
	CK(sx_exec(b, "INSERT INTO u VALUES(2, 'w')", NULL) == SX_OK);
	rc = sx_exec(a, "COMMIT", NULL);
	CK(is_uniq(a, rc));
	CK(one(a, "SELECT count(*) FROM u WHERE k = 'w'") == 1);

	sx_close(a);
	sx_close(b);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_uniq_commit\n");
		return 1;
	}
	printf("  ok   commit-time UNIQUE re-check sees every commit since the "
	    "statement check\n");
	return 0;
}
