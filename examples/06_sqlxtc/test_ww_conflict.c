/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_ww_conflict.c
 *	Write-write conflicts under SNAPSHOT (the default level): two
 *	transactions that both update a row they read from the same
 *	snapshot must not both commit, and the loser fails at its UPDATE
 *	(the row's write intent: first-updater-wins), not at COMMIT; and
 *	SERIALIZABLE must be selectable with SQL (SET TRANSACTION
 *	ISOLATION LEVEL / BEGIN ISOLATION LEVEL / SET SESSION
 *	CHARACTERISTICS).  Before the fix both lost-update shapes below
 *	committed and the balance ended at 120 instead of 130 (or an
 *	abort); the isolation statements were "unsupported SQL".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "engine.h"
#include "xstore.h"
#include "t_tmp.h"

static int
run(sx_db *db, const char *sql)
{
	char *err = NULL;
	int rc = sx_exec(db, sql, &err);
	free(err);
	return rc;
}

static int64_t
bal(sx_db *db)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, "SELECT bal FROM acct WHERE id=1", -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Two txns read bal=100 and add 10 / 20.  Returns the number of the two
 * transactions that committed; *final gets the resulting balance.  The
 * loser fails at its UPDATE (the row's write intent is A's: first-
 * updater-wins, the statement error rolls B back, as PostgreSQL) --
 * never both commit.  *b_stmt_fail reports where B failed. */
static int
race(sx_db *a, sx_db *b, int explicit_value, const char *begin, int64_t *final,
    int *b_stmt_fail)
{
	char q[96];
	int ok = 0, brc;
	int64_t va, vb;
	CK(run(a, "UPDATE acct SET bal=100 WHERE id=1") == SX_OK);
	CK(run(a, begin) == SX_OK);
	CK(run(b, begin) == SX_OK);
	va = bal(a);
	vb = bal(b);
	CK(va == 100 && vb == 100);
	if (explicit_value) {
		snprintf(q, sizeof q, "UPDATE acct SET bal=%lld WHERE id=1", (long long)va + 10);
		CK(run(a, q) == SX_OK);
		snprintf(q, sizeof q, "UPDATE acct SET bal=%lld WHERE id=1", (long long)vb + 20);
		brc = run(b, q);
	} else {
		CK(run(a, "UPDATE acct SET bal=bal+10 WHERE id=1") == SX_OK);
		brc = run(b, "UPDATE acct SET bal=bal+20 WHERE id=1");
	}
	*b_stmt_fail = (brc != SX_OK);
	if (run(a, "COMMIT") == SX_OK) ok++;
	if (run(b, "COMMIT") == SX_OK && brc == SX_OK) ok++;
	*final = bal(a);
	return ok;
}

int
main(void)
{
	char path[256];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *a = NULL, *b = NULL;
	int fd, n;
	int64_t fin;
	const char *shapes[] = { "BEGIN", "BEGIN ISOLATION LEVEL SERIALIZABLE",
	    "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ" };
	int si;

	t_tmpl(path, sizeof path, "sqlxtc-ww");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 128;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK) return 1;
	if (sx_open_bt(bt, &a) != SX_OK || sx_open_bt(bt, &b) != SX_OK) return 1;
	CK(run(a, "CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER)") == SX_OK);
	(void)run(b, "CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER)");
	CK(run(a, "INSERT INTO acct(id,bal) VALUES(1,100)") == SX_OK);

	for (si = 0; si < 3; si++) {
		int bsf = 0;
		n = race(a, b, 0, shapes[si], &fin, &bsf);
		printf("  [%s] bal=bal+N: %d of 2 committed, final %lld, B failed at %s\n",
		    shapes[si], n, (long long)fin, bsf ? "UPDATE" : "COMMIT");
		CK(n == 1);
		CK(fin == 110);      /* first updater (A) wins, B aborts */
		CK(bsf);             /* ... at its statement, not at COMMIT */
		n = race(a, b, 1, shapes[si], &fin, &bsf);
		printf("  [%s] read-modify-write: %d of 2 committed, final %lld, B failed at %s\n",
		    shapes[si], n, (long long)fin, bsf ? "UPDATE" : "COMMIT");
		CK(n == 1);
		CK(fin == 110);
		CK(bsf);
	}
	/* The losing transaction was rolled back: B is usable again and its
	 * retry on fresh data commits. */
	CK(run(b, "UPDATE acct SET bal=bal+20 WHERE id=1") == SX_OK);
	CK(bal(b) == 130);

	/* Autocommit UPDATE vs a txn that updated the same row first: the
	 * autocommit write meets A's intent (off a loop it cannot wait) and
	 * fails; A commits. */
	CK(run(a, "BEGIN") == SX_OK);
	CK(run(a, "UPDATE acct SET bal=bal+1 WHERE id=1") == SX_OK);
	CK(run(b, "UPDATE acct SET bal=bal+2 WHERE id=1") != SX_OK);  /* A holds it */
	CK(run(a, "COMMIT") == SX_OK);
	CK(bal(a) == 131);
	CK(run(b, "UPDATE acct SET bal=bal+1 WHERE id=1") == SX_OK);   /* now free */
	CK(bal(a) == 132);
	/* A txn whose snapshot predates a committed write of the row fails
	 * at its UPDATE (a lost update), not at COMMIT. */
	CK(run(a, "BEGIN") == SX_OK);
	CK(bal(a) == 132);                       /* snapshot taken */
	CK(run(b, "UPDATE acct SET bal=bal+0 WHERE id=1") == SX_OK);   /* commits */
	CK(run(a, "UPDATE acct SET bal=bal+1 WHERE id=1") != SX_OK);
	CK(bal(a) == -1);                        /* block aborted: refused */
	CK(run(a, "COMMIT") != SX_OK);           /* nothing was committed */
	CK(bal(a) == 132);                       /* block ended: usable */

	/* Disjoint rows never conflict. */
	CK(run(a, "INSERT INTO acct(id,bal) VALUES(2,0)") == SX_OK);
	CK(run(a, "BEGIN") == SX_OK);
	CK(run(b, "BEGIN") == SX_OK);
	CK(run(a, "UPDATE acct SET bal=bal+1 WHERE id=1") == SX_OK);
	CK(run(b, "UPDATE acct SET bal=bal+1 WHERE id=2") == SX_OK);
	CK(run(a, "COMMIT") == SX_OK);
	CK(run(b, "COMMIT") == SX_OK);

	/* Isolation statements over SQL. */
	CK(run(a, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE") != SX_OK); /* no txn */
	CK(run(a, "BEGIN") == SX_OK);
	CK(run(a, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE") == SX_OK);
	CK(bal(a) == 133);
	CK(run(a, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED") != SX_OK); /* after a read */
	CK(run(a, "COMMIT") == SX_OK);
	CK(run(a, "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE") == SX_OK);
	CK(run(a, "SET TRANSACTION ISOLATION LEVEL BOGUS") != SX_OK);

	sx_close(a); sx_close(b); bt_close(bt); bm_destroy(bm);
	unlink(path);
	if (g_fail) { fprintf(stderr, "FAIL: test_ww_conflict\n"); return 1; }
	printf("  ok   write-write conflicts: first committer wins under snapshot,"
	    " repeatable read and serializable; isolation settable over SQL\n");
	return 0;
}
