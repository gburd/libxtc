/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_uniq.c
 *	DST: a UNIQUE column holds over keys of every length, under
 *	concurrent writers, and an index lookup finds every row.
 *
 *	Per seed, inside xtc_sim_exec_run on N_LOOPS loops: a table with a
 *	UNIQUE TEXT column and a plain index on the same column.  NW writer
 *	procs, each with its own connection, insert values drawn from a
 *	small seeded pool, so most INSERTs collide with another writer's.
 *	The pool mixes short values, values just over bt_max_key (497 on
 *	4 KiB pages) and up to 3000 bytes, and pairs that share their first
 *	490 bytes, so long values meet both duplicates and near-duplicates.
 *	Half the writes run in BEGIN..COMMIT (the commit-time re-check),
 *	half autocommit; some UPDATE an existing row to a pool value; each
 *	writer parks a seeded few microseconds between statements.  Before
 *	one write in eight a writer arms xstore_uniq_fail_inject(1), so the
 *	next UNIQUE probe of the committed index (any writer's) fails as a
 *	page read can, and before one in eight (drawn from the fault stream,
 *	so the application draws stay as they were) it arms
 *	xstore_index_read_fail_at, so one of the next four B-tree reads of a
 *	probe fails, its candidate-row read among them.  A writer that sees
 *	"UNIQUE constraint failed: t.s" or "index error: cannot check UNIQUE
 *	t.s" carries on (the write was refused); any other error fails the
 *	run.  The table also holds NGRP rows with one 200-byte value of g
 *	under a plain index on g: their entries span several leaves.  While
 *	the writers run, a mover proc inserts and deletes rows of that
 *	group (rowids between the group's own) and, three times in four,
 *	moves a group row out of it and back by UPDATE.  It changes only
 *	the first NHOT rows: an equality probe's first batch ends among
 *	them, so the entry it hands to the cursor is often one the mover
 *	has just deleted.  A reader proc counts the group twice through the
 *	index and once by a table scan inside one transaction (one
 *	snapshot).  Buggify is on:
 *	the site xstore.iscan.handoff stalls an equality probe between its
 *	first leaf's batch and the cursor that reads the rest, so the
 *	mover's writes land in that window.  A reader statement that fails
 *	with "index error: cannot read index ...: I/O error" while the fault
 *	hook failed a read (a writer's armed fault that the reader drew) is
 *	skipped; any other reader error fails the run.
 *
 *	Then, once the writers are done, a second phase on a table c:
 *	NCTR counter procs each add 1 to one row NINC times, each time in a
 *	transaction that reads the row, parks, and writes it back, and a
 *	pk proc tries NPK times to INSERT, in a transaction, a key that c
 *	already holds.  Before about half their statements they arm
 *	xstore_row_read_fail_at (from the fault stream), so one of the next
 *	reads of one row outside a scan fails: the INSERT's key check, an
 *	UPDATE's write-write check at its write intent, a COMMIT's.  A
 *	statement that fails with "cannot read table c: I/O error" while
 *	that hook failed a read is fine (it changed nothing); so is a
 *	refused duplicate or a serialization failure.  The checker then
 *	counts the g group by a table scan with xstore_scan_read_fail_at
 *	armed: a count that fails with "cannot read table t: I/O error" is
 *	retried unarmed.  Last, with no other proc running: NAUTO
 *	transactions INSERT a row with no key into a fresh table with the
 *	row fault armed, and NREAP times a superseded UNIQUE entry that a
 *	newer version uses again is reaped with a read (drawn from the
 *	fault stream) failing.
 *
 *	Invariants (each a FAIL line naming the seed):
 *	  UNIQUE   no two live rows carry one value (counted by a table scan
 *	           with the index defeated);
 *	  INDEX    for every pool value, its count through the index equals
 *	           its count by the scan, so does the g group's (an equality
 *	           probe over many leaves), and xstore_index_check reports
 *	           no entry missing;
 *	  SNAPSHOT the reader's two index counts and its scan count of the
 *	           g group under one snapshot are equal;
 *	  BUGGIFY  over 8 or more seeds, xstore.iscan.handoff fired at least
 *	           once (the window was exercised);
 *	  SCAN     a table scan whose read failed never returns a count (a
 *	           short scan is never the whole table);
 *	  PKEY     no INSERT of a key c holds commits;
 *	  LOST UPDATE the counter equals the increments acknowledged;
 *	  ROWID    an auto-rowid INSERT never overwrites a committed row;
 *	  REAP     a reaped UNIQUE entry a live row uses is never lost (a
 *	           duplicate of its value is refused);
 *	  REPLAY   the same seed gives the same result text.
 *
 *	Planted bugs (scripts/dst-bug-inject.sh): 72 (UNIQPROBE) reads a
 *	failed UNIQUE probe as "no duplicate", as 2.2.0 did, so a long
 *	duplicate is accepted; 73 (LONGKEY) skips the entry of a key
 *	longer than one B-tree key, as 2.2.0 did, so the index misses rows;
 *	80 (UNIQREAD) reads a UNIQUE candidate row that could not be read
 *	as absent, so a long duplicate is accepted; 81 (POINTEND) ends an
 *	equality index probe with its first leaf, so the g group is short
 *	(the reader's SNAPSHOT check or the checker's INDEX check); 82
 *	(SCANSHORT) takes a failed base-scan read for the table's end (SCAN);
 *	83 (PKREAD) reads a row the INSERT key check cannot read as absent
 *	(PKEY); 84 (WWREAD) reads a row the write-write check cannot read as
 *	having no newer version, so an increment is lost (LOST UPDATE); 85
 *	(MAXROWID) reads a failed max-rowid descent as an empty table, so an
 *	auto-rowid INSERT takes rowid 1 (ROWID); 86 (REAPREAD) reads a failed
 *	step of the reap's reuse check as "not reused" (REAP).
 *	usage: test_sim_sqlxtc_uniq [base [count]]   (default 1 8)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_runtime.h"
#include "sim_int.h"

#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "xstore.h"

#define N_LOOPS 3
#define NW 4     /* writer procs */
#define NOPS 24  /* statements per writer */
#define NPOOL 10 /* distinct values */
#define VMAX 3000
#define OUTCAP 2048
#define NGRP 60 /* rows of the g group: about 17 entries per leaf */
#define GRP_K 900000 /* the group's rowids: GRP_K + 2i; the mover's odd */
#define NHOT 24 /* the group rows the mover changes: the first leaves */
#define NMOVE 120 /* mover statements */
#define NREAD 40 /* reader transactions */
#define RD_ERR "index error: cannot read index "
#define RD_EIO ": I/O error" /* how RD_ERR ends for an injected fault */
#define TB_ERR "cannot read table " /* a row or scan read error */
#define NCTR 2   /* counter procs */
#define NINC 16  /* increments each tries */
#define NPK 8    /* duplicate-key INSERTs the pk proc tries */
#define NAUTO 8  /* auto-rowid INSERT transactions (phase 2) */
#define NREAP 4  /* UNIQUE reaps of a reused entry (phase 2) */

static bt_t *g_bt;
static char g_pool[NPOOL][VMAX + 1];
static char g_grp[201]; /* the g group's value */
static char g_out[OUTCAP];
static char g_why[256];
static int g_left, g_ok, g_handoff;
static long g_dups; /* UNIQUE refusals seen */
static long g_perr; /* writes refused by a failed probe */
static int g_left2;  /* phase-2 procs still running */
static long g_incs;  /* increments acknowledged */
static long g_rerr;  /* phase-2 statements refused by a failed read */

/* Value i of the pool: a short one, or 498..3000 bytes of a letter with
 * a tag at the end (so values 2k and 2k + 1 of one length share all but
 * their last 8 bytes). */
static void
pool_make(void)
{
	static const int len[NPOOL] = { 3, 7, 498, 498, 699, 699, 1200, 1200,
		3000, 3000 };
	int i;
	for (i = 0; i < NPOOL; i++) {
		if (len[i] < 8) {
			snprintf(g_pool[i], sizeof g_pool[i], "s%d", i);
			continue;
		}
		memset(g_pool[i], 'a' + i / 2, (size_t)len[i]);
		snprintf(g_pool[i] + len[i] - 8, 9, "%08d", i);
	}
}

static uint64_t
draw(uint64_t n)
{
	return __xtc_sim_rng_range(XTC_SIM_RNG_APP, n);
}

/* Run sql with up to two binds (an int64 k, then a pool value v; -1 for
 * none).  Returns the step's code. */
static int
run(sx_db *h, const char *sql, int64_t k, int v)
{
	sx_stmt *st = NULL;
	int rc, b = 1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (v >= 0)
		sx_bind_text(st, b++, g_pool[v], -1);
	if (k >= 0)
		sx_bind_int64(st, b, k);
	rc = sx_step(st);
	sx_finalize(st);
	return rc;
}

/* Run `sql` with the g group's value bound to its ? (if any). */
static int
grp_exec(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int rc;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (strchr(sql, '?') != NULL)
		sx_bind_text(st, 1, g_grp, -1);
	rc = sx_step(st);
	sx_finalize(st);
	return rc;
}

/* `sql` (one bind: the g group's value) as a count, -2 on error. */
static int64_t
grp_count(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t n = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	sx_bind_text(st, 1, g_grp, -1);
	if (sx_step(st) == SX_ROW)
		n = sx_column_int64(st, 0);
	sx_finalize(st);
	return n;
}

static int64_t
count_q(sx_db *h, const char *sql, int v)
{
	sx_stmt *st = NULL;
	int64_t n = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	sx_bind_text(st, 1, g_pool[v], -1);
	if (sx_step(st) == SX_ROW)
		n = sx_column_int64(st, 0);
	sx_finalize(st);
	return n;
}

/* A failed statement is fine only if it is a UNIQUE refusal. */
static int
uniq_or_fail(sx_db *h, int rc, const char *what)
{
	const char *m = sx_errmsg(h);
	if (rc == SX_DONE || rc == SX_OK)
		return 0;
	if (m != NULL && strstr(m, "UNIQUE constraint failed: t.s") != NULL) {
		g_dups++;
		return 0;
	}
	if (m != NULL &&
	    strstr(m, "index error: cannot check UNIQUE t.s") != NULL) {
		g_perr++;
		return 0;
	}
	if (g_why[0] == '\0')
		snprintf(g_why, sizeof g_why, "%s failed: rc %d \"%s\"", what,
		    rc, m != NULL ? m : "");
	return -1;
}

static void
writer(void *arg)
{
	long me = (long)arg;
	sx_db *h = NULL;
	int i, v, rc, txn;
	int64_t k;
	if (sx_open_bt(g_bt, &h) != SX_OK)
		goto out;
	for (i = 0; i < NOPS && g_why[0] == '\0'; i++) {
		v = (int)draw(NPOOL);
		k = me * 1000 + i;
		txn = (int)draw(2);
		if (txn && sx_exec(h, "BEGIN", NULL) != SX_OK)
			break;
		if (draw(8) == 0)
			xstore_uniq_fail_inject(1);
		if (__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 8) == 0)
			xstore_index_read_fail_at(
			    1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 4));
		if (draw(4) == 0)
			rc = run(h, "UPDATE t SET s = ? WHERE k = ?",
			    (me * 1000) + (int64_t)draw((uint64_t)i + 1), v);
		else
			rc = run(h, "INSERT INTO t(s, k) VALUES(?, ?)", k, v);
		if (uniq_or_fail(h, rc, "write") != 0)
			break;
		(void)xtc_proc_sleep((int64_t)(1 + draw(300)) * 1000);
		if (txn) {
			rc = sx_exec(h, rc == SX_DONE ? "COMMIT" : "ROLLBACK",
			    NULL);
			if (uniq_or_fail(h, rc, "COMMIT") != 0)
				break;
		}
	}
out:
	if (h != NULL)
		sx_close(h);
	xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst);
}

/* The mover: inserts and deletes rows of the g group at rowids between
 * the group's own, and moves group rows out of it and back, then puts
 * the group back as it was. */
static void
mover(void *arg)
{
	sx_db *h = NULL;
	char sql[128];
	unsigned char ins[NGRP] = { 0 }, out[NGRP] = { 0 };
	int i, j, op, rc = SX_DONE;
	(void)arg;
	if (sx_open_bt(g_bt, &h) != SX_OK)
		goto done;
	for (i = 0; i < NMOVE && g_why[0] == '\0' && rc == SX_DONE; i++) {
		j = (int)draw(NHOT);
		op = draw(4) != 0; /* 1 (a move): three times in four */
		if (op == 0 && !ins[j]) {
			snprintf(sql, sizeof sql,
			    "INSERT INTO t(k, g) VALUES(%d, ?)",
			    GRP_K + 2 * j + 1);
			ins[j] = 1;
		} else if (op == 0) {
			snprintf(sql, sizeof sql, "DELETE FROM t WHERE k = %d",
			    GRP_K + 2 * j + 1);
			ins[j] = 0;
		} else {
			snprintf(sql, sizeof sql,
			    "UPDATE t SET g = %s WHERE k = %d",
			    out[j] ? "?" : "'out'", GRP_K + 2 * j);
			out[j] = !out[j];
		}
		rc = grp_exec(h, sql);
		(void)xtc_proc_sleep((int64_t)(1 + draw(800)) * 1000);
	}
	for (j = 0; j < NGRP && rc == SX_DONE; j++) {
		if (ins[j]) {
			snprintf(sql, sizeof sql, "DELETE FROM t WHERE k = %d",
			    GRP_K + 2 * j + 1);
			rc = grp_exec(h, sql);
		}
		if (out[j] && rc == SX_DONE) {
			snprintf(sql, sizeof sql,
			    "UPDATE t SET g = ? WHERE k = %d", GRP_K + 2 * j);
			rc = grp_exec(h, sql);
		}
	}
	if (rc != SX_DONE && g_why[0] == '\0')
		snprintf(g_why, sizeof g_why,
		    "mover: \"%s\" failed: rc %d \"%s\"", sql, rc,
		    sx_errmsg(h));
done:
	if (h != NULL)
		sx_close(h);
	xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst);
}

/* The reader's count of the g group by `sql`.  *f becomes 1 if the
 * count failed on an injected read fault: the fault hook failed a read
 * while it ran, and the statement says it could not read the index with
 * an I/O error.  Any other failure sets *f to -1 and fails the run. */
static int64_t
rd_count(sx_db *h, const char *sql, int *f)
{
	int f0 = xstore_index_read_faults();
	int64_t n = grp_count(h, sql);
	const char *m = sx_errmsg(h);
	size_t ml = m != NULL ? strlen(m) : 0;
	if (n != -2 || *f < 0)
		return n;
	if (xstore_index_read_faults() != f0 && m != NULL &&
	    strncmp(m, RD_ERR, sizeof RD_ERR - 1) == 0 &&
	    ml >= sizeof RD_EIO - 1 &&
	    strcmp(m + ml - (sizeof RD_EIO - 1), RD_EIO) == 0) {
		*f = 1;
		return n;
	}
	*f = -1;
	if (g_why[0] == '\0')
		snprintf(g_why, sizeof g_why, "reader: count failed: \"%s\"",
		    m != NULL ? m : "");
	return n;
}

/* The reader: the g group counted twice through the index and once by a
 * scan, under one snapshot, while the mover changes the group. */
static void
reader(void *arg)
{
	sx_db *h = NULL;
	int64_t a, b, c;
	int i, f;
	(void)arg;
	if (sx_open_bt(g_bt, &h) != SX_OK)
		goto done;
	for (i = 0; i < NREAD && g_why[0] == '\0'; i++) {
		if (sx_exec(h, "BEGIN", NULL) != SX_OK)
			break;
		f = 0;
		a = rd_count(h, "SELECT count(*) FROM t WHERE g = ?", &f);
		(void)xtc_proc_sleep((int64_t)(1 + draw(500)) * 1000);
		b = rd_count(h, "SELECT count(*) FROM t WHERE g = ?", &f);
		c = rd_count(h, "SELECT count(*) FROM t WHERE g || '' = ?", &f);
		(void)sx_exec(h, "COMMIT", NULL);
		if (f < 0)
			break;
		if (f == 0 && (a != b || b != c) && g_why[0] == '\0')
			snprintf(g_why, sizeof g_why,
			    "SNAPSHOT: the g group under one snapshot: index "
			    "%lld then %lld, scan %lld",
			    (long long)a, (long long)b, (long long)c);
		(void)xtc_proc_sleep((int64_t)(1 + draw(300)) * 1000);
	}
done:
	if (h != NULL)
		sx_close(h);
	xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst);
}

/* The single integer of `sql`, or -2. */
static int64_t
one_int(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Did a statement fail on an injected row read: the row-read hook
 * failed a read since f0, and the message names the table it could not
 * read with an I/O error? */
static int
row_fault_ok(sx_db *h, int f0)
{
	const char *m = sx_errmsg(h);
	size_t ml = m != NULL ? strlen(m) : 0;
	return xstore_row_read_faults() != f0 && m != NULL &&
	    strncmp(m, TB_ERR, sizeof TB_ERR - 1) == 0 &&
	    ml >= sizeof RD_EIO - 1 &&
	    strcmp(m + ml - (sizeof RD_EIO - 1), RD_EIO) == 0;
}

/* Before about half of phase 2's statements: fail its first row read
 * (or, one time in four, its second). */
static void
arm_row_fault(void)
{
	if (__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 2) == 0)
		xstore_row_read_fail_at(
		    __xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 4) == 0 ? 2 : 1);
}

/* A counter proc: NINC increments of c's row 1, each a transaction
 * that reads the row, parks, then writes it, so another counter's
 * commit often lands in between and the write-write check must refuse
 * it.  The row-read fault is armed before the UPDATE and before the
 * COMMIT, the two places that check reads the row. */
static void
counter(void *arg)
{
	sx_db *h = NULL;
	int i, rc, f0;
	(void)arg;
	if (sx_open_bt(g_bt, &h) != SX_OK)
		goto done;
	for (i = 0; i < NINC && g_why[0] == '\0'; i++) {
		if (sx_exec(h, "BEGIN", NULL) != SX_OK ||
		    one_int(h, "SELECT v FROM c WHERE k = 1") < 0)
			break;
		(void)xtc_proc_sleep((int64_t)(1 + draw(300)) * 1000);
		f0 = xstore_row_read_faults();
		arm_row_fault();
		rc = sx_exec(h, "UPDATE c SET v = v + 1 WHERE k = 1", NULL);
		if (rc == SX_OK) {
			arm_row_fault();
			rc = sx_exec(h, "COMMIT", NULL);
		}
		if (rc == SX_OK)
			g_incs++;
		else if (row_fault_ok(h, f0))
			g_rerr++;
		else if (rc != SX_BUSY && g_why[0] == '\0')
			snprintf(g_why, sizeof g_why, "counter: rc %d \"%s\"",
			    rc, sx_errmsg(h));
		if (rc != SX_OK)
			(void)sx_exec(h, "ROLLBACK", NULL);
		xstore_row_read_fail_at(0);
		(void)xtc_proc_sleep((int64_t)(1 + draw(200)) * 1000);
	}
done:
	if (h != NULL)
		sx_close(h);
	xtc_atomic_fetch_sub_i32(&g_left2, 1, xtc_mo_seq_cst);
}

/* The pk proc: NPK transactions that INSERT key 2, which c holds. */
static void
pkdup(void *arg)
{
	sx_db *h = NULL;
	char sql[64];
	int i, rc, f0;
	(void)arg;
	if (sx_open_bt(g_bt, &h) != SX_OK)
		goto done;
	for (i = 0; i < NPK && g_why[0] == '\0'; i++) {
		if (sx_exec(h, "BEGIN", NULL) != SX_OK)
			break;
		arm_row_fault();
		f0 = xstore_row_read_faults();
		snprintf(sql, sizeof sql, "INSERT INTO c VALUES(2, %d)",
		    100 + i);
		rc = sx_exec(h, sql, NULL);
		if (rc == SX_OK)
			rc = sx_exec(h, "COMMIT", NULL);
		/* Judge the failure before the ROLLBACK: a statement that
		 * succeeds clears the connection's message. */
		if (rc == SX_OK && g_why[0] == '\0')
			snprintf(g_why, sizeof g_why, "PKEY: an INSERT of "
			    "existing key 2 was committed (try %d)", i);
		else if (rc != SX_OK && row_fault_ok(h, f0))
			g_rerr++;
		else if (rc != SX_CONSTRAINT && rc != SX_BUSY &&
		    g_why[0] == '\0')
			snprintf(g_why, sizeof g_why, "pk: rc %d \"%s\"", rc,
			    sx_errmsg(h));
		if (rc != SX_OK)
			(void)sx_exec(h, "ROLLBACK", NULL);
		(void)xtc_proc_sleep((int64_t)(1 + draw(300)) * 1000);
	}
done:
	if (h != NULL)
		sx_close(h);
	xtc_atomic_fetch_sub_i32(&g_left2, 1, xtc_mo_seq_cst);
}

/* Phase 2, auto-rowid: NAUTO transactions, each on a fresh table a<j>
 * holding rows 1 and 2 (explicit keys, so no rowid has been handed out
 * for it), INSERT a row with no key and COMMIT, with the row fault
 * armed.  The INSERT takes its rowid past the table's largest; a read
 * of that which failed must fail the statement, never take rowid 1 and
 * overwrite a committed row (ROWID). */
static void
phase2_autorowid(sx_db *h)
{
	char sql[96];
	int j, rc, f0;
	for (j = 0; j < NAUTO && g_why[0] == '\0'; j++) {
		snprintf(sql, sizeof sql, "CREATE TABLE a%d(k INTEGER PRIMARY "
		    "KEY, v INTEGER)", j);
		if (sx_exec(h, sql, NULL) != SX_OK)
			break;
		snprintf(sql, sizeof sql, "INSERT INTO a%d VALUES(1, 1), (2, 2)",
		    j);
		if (sx_exec(h, sql, NULL) != SX_OK ||
		    sx_exec(h, "BEGIN", NULL) != SX_OK)
			break;
		f0 = xstore_row_read_faults();
		arm_row_fault();
		snprintf(sql, sizeof sql, "INSERT INTO a%d(v) VALUES(9)", j);
		rc = sx_exec(h, sql, NULL);
		if (rc == SX_OK)
			rc = sx_exec(h, "COMMIT", NULL);
		xstore_row_read_fail_at(0);
		if (rc != SX_OK && row_fault_ok(h, f0))
			g_rerr++;
		else if (rc != SX_OK && g_why[0] == '\0')
			snprintf(g_why, sizeof g_why, "autorowid: rc %d \"%s\"",
			    rc, sx_errmsg(h));
		if (rc != SX_OK)
			(void)sx_exec(h, "ROLLBACK", NULL);
		snprintf(sql, sizeof sql, "SELECT v FROM a%d WHERE k = 1", j);
		if (g_why[0] == '\0' && one_int(h, sql) != 1)
			snprintf(g_why, sizeof g_why, "ROWID: committed row 1 of "
			    "a%d overwritten (holds %lld)", j,
			    (long long)one_int(h, sql));
		snprintf(sql, sizeof sql, "DROP TABLE a%d", j);
		(void)sx_exec(h, sql, NULL);
	}
}

/* Phase 2, the UNIQUE reap: NREAP times, on a fresh table r<j>, row 1's
 * UNIQUE value goes 10 -> 20 -> 10 while a REPEATABLE READ snapshot
 * pins the first version, so the superseded entry (10, 1) is queued for
 * the reap; the pin ends, and four INSERTs run with the n-th row read
 * failing (n drawn from the fault stream), so the reap that checks
 * whether a newer version uses the entry again can fail a read.  The
 * entry is live (row 1 holds 10 again): a duplicate 10 must still be
 * refused (REAP). */
static void
phase2_reap(sx_db *h)
{
	sx_db *pin = NULL;
	char sql[96];
	int j, i, rc;
	for (j = 0; j < NREAP && g_why[0] == '\0'; j++) {
		snprintf(sql, sizeof sql, "CREATE TABLE r%d(k INTEGER PRIMARY "
		    "KEY, c INTEGER UNIQUE)", j);
		if (sx_exec(h, sql, NULL) != SX_OK)
			break;
		snprintf(sql, sizeof sql, "INSERT INTO r%d VALUES(1, 10)", j);
		if (sx_exec(h, sql, NULL) != SX_OK ||
		    sx_open_bt(g_bt, &pin) != SX_OK)
			break;
		snprintf(sql, sizeof sql, "SELECT c FROM r%d WHERE k = 1", j);
		if (sx_exec(pin, "BEGIN ISOLATION LEVEL REPEATABLE READ",
		        NULL) != SX_OK || one_int(pin, sql) != 10)
			break;
		snprintf(sql, sizeof sql, "UPDATE r%d SET c = 20 WHERE k = 1", j);
		(void)sx_exec(h, sql, NULL);
		snprintf(sql, sizeof sql, "UPDATE r%d SET c = 10 WHERE k = 1", j);
		(void)sx_exec(h, sql, NULL);
		(void)sx_exec(pin, "COMMIT", NULL);
		sx_close(pin);
		pin = NULL;
		xstore_row_read_fail_at(
		    1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 12));
		for (i = 0; i < 4; i++) {        /* may fail on the read: fine */
			snprintf(sql, sizeof sql, "INSERT INTO r%d VALUES(%d, %d)",
			    j, 100 + i, 100 + i);
			(void)sx_exec(h, sql, NULL);
		}
		xstore_row_read_fail_at(0);
		snprintf(sql, sizeof sql, "INSERT INTO r%d VALUES(2, 10)", j);
		rc = sx_exec(h, sql, NULL);
		if (rc == SX_OK)
			snprintf(g_why, sizeof g_why, "REAP: a duplicate of the "
			    "live UNIQUE value 10 went into r%d", j);
		else if (rc != SX_CONSTRAINT && g_why[0] == '\0')
			snprintf(g_why, sizeof g_why, "reap: rc %d \"%s\"", rc,
			    sx_errmsg(h));
		snprintf(sql, sizeof sql, "DROP TABLE r%d", j);
		(void)sx_exec(h, sql, NULL);
	}
	if (pin != NULL)
		sx_close(pin);
}

/* Phase 2 (from the checker, once the writers are done): the counter
 * and pk procs over table c, then their invariants; then the
 * auto-rowid and reap cases, alone. */
static void
phase2(xtc_exec_t *e, sx_db *h)
{
	int64_t v, n, k2;
	int i;
	if (sx_exec(h, "CREATE TABLE c(k INTEGER PRIMARY KEY, v INTEGER)",
	        NULL) != SX_OK ||
	    sx_exec(h, "INSERT INTO c VALUES(1, 0), (2, 7)", NULL) != SX_OK) {
		snprintf(g_why, sizeof g_why, "phase 2: create failed");
		return;
	}
	xtc_atomic_store_i32(&g_left2, NCTR + 1, xtc_mo_seq_cst);
	for (i = 0; i < NCTR; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, 1 + i % (N_LOOPS - 1)),
		    counter, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), pkdup, NULL, NULL, NULL);
	while (xtc_atomic_load_i32(&g_left2, xtc_mo_seq_cst) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	xstore_row_read_fail_at(0); /* a fault no statement reached */
	if (g_why[0] != '\0')
		return;
	v = one_int(h, "SELECT v FROM c WHERE k = 1");
	n = one_int(h, "SELECT count(*) FROM c");
	k2 = one_int(h, "SELECT v FROM c WHERE k = 2");
	if (v != g_incs)
		snprintf(g_why, sizeof g_why, "LOST UPDATE: counter %lld, %ld "
		    "increments acknowledged", (long long)v, g_incs);
	else if (n != 2 || k2 != 7)
		snprintf(g_why, sizeof g_why, "PKEY: key 2 holds %lld (%lld "
		    "rows), want 7", (long long)k2, (long long)n);
	if (g_why[0] != '\0')
		return;
	phase2_autorowid(h);
	phase2_reap(h);
}

/* The g group counted by a table scan with one of its reads failing:
 * a count is wrong (the scan was short), a failure is retried with no
 * fault.  -2 on another error. */
static int64_t
scan_count(sx_db *h)
{
	int64_t b;
	int left, tries;
	const char *m;
	for (tries = 0; tries < 4; tries++) {
		xstore_scan_read_fail_at(tries < 3 ?
		    1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 4) : 0);
		b = grp_count(h, "SELECT count(*) FROM t WHERE g || '' = ?");
		left = xstore_scan_read_fail_at(0);
		if (tries == 3 || left != 0)
			return b;                 /* no read failed */
		if (b != -2) {
			snprintf(g_why, sizeof g_why, "SCAN: a scan whose read "
			    "failed returned %lld rows of the g group",
			    (long long)b);
			return b;
		}
		m = sx_errmsg(h);
		if (m == NULL || strncmp(m, TB_ERR, sizeof TB_ERR - 1) != 0) {
			snprintf(g_why, sizeof g_why, "SCAN: failed: \"%s\"",
			    m != NULL ? m : "");
			return -2;
		}
	}
	return -2;
}

/* After the writers: the invariants, and the result text. */
static void
checker(void *arg)
{
	xtc_exec_t *e = arg;
	sx_db *h = NULL;
	uint64_t miss = 0, stale = 0;
	int64_t a, b;
	size_t l;
	int v;
	while (xtc_atomic_load_i32(&g_left, xtc_mo_seq_cst) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	xstore_index_read_fail_at(0); /* a fault no writer reached */
	if (g_why[0] != '\0' || sx_open_bt(g_bt, &h) != SX_OK)
		goto out;
	phase2(e, h);
	if (g_why[0] != '\0')
		goto out;
	a = grp_count(h, "SELECT count(*) FROM t WHERE g = ?");
	b = scan_count(h);
	if (g_why[0] != '\0')
		goto out;
	if (a != b || b != NGRP) {
		snprintf(g_why, sizeof g_why,
		    "INDEX: the g group: index %lld, scan %lld, want %d",
		    (long long)a, (long long)b, NGRP);
		goto out;
	}
	for (v = 0; v < NPOOL; v++) {
		a = count_q(h, "SELECT count(*) FROM t WHERE s = ?", v);
		b = count_q(h, "SELECT count(*) FROM t WHERE s || '' = ?", v);
		if (b > 1) {
			snprintf(g_why, sizeof g_why,
			    "UNIQUE: value %d (%d bytes) "
			    "is held by %lld rows",
			    v, (int)strlen(g_pool[v]), (long long)b);
			goto out;
		}
		if (a != b) {
			snprintf(g_why, sizeof g_why,
			    "INDEX: value %d (%d bytes): "
			    "index %lld, scan %lld",
			    v, (int)strlen(g_pool[v]), (long long)a,
			    (long long)b);
			goto out;
		}
		l = strlen(g_out);
		snprintf(g_out + l, OUTCAP - l, "%lld,", (long long)b);
	}
	(void)xstore_index_check(g_bt, &miss, &stale);
	if (miss != 0) {
		snprintf(g_why, sizeof g_why,
		    "INDEX: xstore_index_check: %llu "
		    "entries missing",
		    (unsigned long long)miss);
		goto out;
	}
	l = strlen(g_out);
	snprintf(g_out + l, OUTCAP - l, "d%ld,e%ld,c%ld,r%ld", g_dups, g_perr,
	    g_incs, g_rerr);
	g_ok = 1;
out:
	if (h != NULL) {
		(void)sx_exec(h, "DROP TABLE IF EXISTS t", NULL);
		(void)sx_exec(h, "DROP TABLE IF EXISTS c", NULL);
		sx_close(h);
	}
	(void)xtc_exec_stop(e);
}

/* The g group: NGRP rows with one long value of g, past every rowid a
 * writer uses.  0, or -1 on a failed insert. */
static int
grp_fill(sx_db *h)
{
	sx_stmt *st = NULL;
	int i, rc = 0;
	if (sx_prepare(h, "INSERT INTO t(k, g) VALUES(?, ?)", -1, &st, NULL) !=
	    SX_OK)
		return -1;
	for (i = 0; i < NGRP && rc == 0; i++) {
		sx_bind_int64(st, 1, GRP_K + 2 * i);
		sx_bind_text(st, 2, g_grp, -1);
		rc = sx_step(st) == SX_DONE ? 0 : -1;
		sx_reset(st);
	}
	sx_finalize(st);
	return rc;
}

static void
setup(void *arg)
{
	xtc_exec_t *e = arg;
	sx_db *h = NULL;
	long i;
	if (sx_open_bt(g_bt, &h) != SX_OK ||
	    sx_exec(h,
	        "CREATE TABLE t(k INTEGER PRIMARY KEY, s TEXT UNIQUE, g TEXT)",
	        NULL) != SX_OK ||
	    sx_exec(h, "CREATE INDEX t_s ON t(s)", NULL) != SX_OK ||
	    sx_exec(h, "CREATE INDEX t_g ON t(g)", NULL) != SX_OK ||
	    grp_fill(h) != 0) {
		snprintf(g_why, sizeof g_why, "create failed");
		if (h != NULL)
			sx_close(h);
		(void)xtc_exec_stop(e);
		return;
	}
	sx_close(h);
	xtc_atomic_store_i32(&g_left, NW + 2, xtc_mo_seq_cst);
	for (i = 0; i < NW; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (int)(i % N_LOOPS)),
		    writer, (void *)i, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 2), mover, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), reader, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), checker, e, NULL, NULL);
}

/* 1 if this run's xstore.iscan.handoff site was reached and active. */
static int
handoff_fired(void)
{
	char name[64];
	int i, on = 0;
	for (i = 0; xtc_sim_buggify_site(i, name, sizeof name, &on) == XTC_OK;
	    i++)
		if (strcmp(name, "xstore.iscan.handoff") == 0)
			return on;
	return 0;
}

static int
run_one(uint64_t seed, char *out)
{
	xtc_exec_t *e = NULL;
	int rc;
	g_out[0] = '\0';
	g_why[0] = '\0';
	g_ok = 0;
	g_dups = g_perr = 0;
	g_incs = g_rerr = 0;
	if (xtc_exec_create(&(xtc_exec_opts_t) { .size =
	                                             sizeof(xtc_exec_opts_t),
	                        .n_loops = N_LOOPS },
	        &e) != XTC_OK)
		return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), setup, e, NULL, NULL);
	xtc_sim_buggify_enable(500);
	rc = xtc_sim_exec_run(e, seed, 50000000);
	g_handoff += handoff_fired();
	xtc_sim_buggify_disable();
	xstore_uniq_fail_inject(0); /* an armed failure outlived the run */
	xstore_index_read_fail_at(0);
	xstore_row_read_fail_at(0);
	xstore_scan_read_fail_at(0);
	xtc_exec_destroy(e);
	snprintf(out, OUTCAP, "%s", g_out);
	return (rc == XTC_OK && g_ok) ? 0 : -1;
}

/* A fresh, empty tree in the same page file, and the commit clock back
 * at its start (see test_sim_sqlxtc_gcidx). */
static int
fresh(bm_t **bm, bm_opts_t *bo)
{
	if (g_bt != NULL)
		bt_close(g_bt);
	if (*bm != NULL)
		bm_destroy(*bm);
	g_bt = NULL;
	*bm = NULL;
	xstore_clock_reset();
	bo->reopen = 0;
	if (truncate(bo->path, 0) != 0 || bm_create(bo, bm) != XTC_OK ||
	    bt_open(*bm, &g_bt) != XTC_OK)
		return -1;
	return 0;
}

int
main(int argc, char **argv)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	char btf[512];
	const char *td = getenv("TMPDIR");
	static char a[OUTCAP], b[OUTCAP];
	uint64_t base = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
	uint64_t count = argc > 2 ? strtoull(argv[2], NULL, 0) : 8, s;
	int fd, fails = 0;

	/* SQL on a fiber needs sqlxtc's 512 KiB stack (main.c). */
	if (xtc_runtime_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	snprintf(btf, sizeof btf, "%s/xtc-uniq-bt-XXXXXX",
	    td != NULL && td[0] != '\0' ? td : "/tmp");
	fd = mkstemp(btf);
	if (fd < 0)
		return 1;
	close(fd);
	pool_make();
	memset(g_grp, 'g', sizeof g_grp - 1);
	bo.path = btf;
	bo.page_size = 4096;
	bo.n_frames = 512;
	for (s = base; s < base + count; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (s + 1);
		int ra, rb;
		ra = fresh(&bm, &bo) != 0 ? -1 : run_one(seed, a);
		rb = ra != 0 || fresh(&bm, &bo) != 0 ? -1 : run_one(seed, b);
		if (ra != 0 || rb != 0) {
			printf("  FAIL: seed=%llu (0x%016llx): %s\n",
			    (unsigned long long)s, (unsigned long long)seed,
			    g_why[0] ? g_why : "run error or guard trip");
			printf("  corpus row: test_sim_sqlxtc_uniq %llu 1 "
			       "<description, fixing commit>\n",
			    (unsigned long long)s);
			fails++;
			continue;
		}
		if (strcmp(a, b) != 0) {
			printf(
			    "  FAIL: seed=%llu: REPLAY: two runs differ (%s vs "
			    "%s)\n",
			    (unsigned long long)s, a, b);
			fails++;
		}
	}
	if (g_bt != NULL)
		bt_close(g_bt);
	if (bm != NULL)
		bm_destroy(bm);
	unlink(btf);
	if (fails == 0 && count >= 8 && g_handoff == 0) {
		printf("  FAIL: BUGGIFY: xstore.iscan.handoff never fired in "
		       "%llu seeds\n", (unsigned long long)count);
		fails++;
	}
	if (fails) {
		printf("FAIL: %d of %llu seeds\n", fails,
		    (unsigned long long)count);
		return 1;
	}
	printf("OK: UNIQUE holds and the index finds every row over keys of "
	       "3..3000 bytes -- %llu seeds, %d writers, replay clean, "
	       "hand-off buggified in %d runs\n",
	    (unsigned long long)count, NW, g_handoff);
	return 0;
}
