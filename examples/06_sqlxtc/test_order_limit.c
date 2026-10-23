/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_order_limit.c
 *	ORDER BY ... LIMIT must return the first rows of the unlimited
 *	ORDER BY, in the same order, with equal keys in scan order --
 *	SQLite's rule (its sorter breaks ties on an arrival counter).
 *	Rows are compared IN THE ORDER RETURNED: the socket oracle sorts
 *	both sides before comparing and cannot see a tie-order bug.
 *
 *	  1. 19 shapes over a 10-row table with ties and NULLs (ASC, DESC,
 *	     two keys, NULLS FIRST/LAST, DISTINCT + LIMIT, OFFSET, LIMIT 0,
 *	     an expression key, a key that is not an output column); the
 *	     expected strings are SQLite 3.53.4's, and 3.45.1 gives the
 *	     same bytes on every one.
 *	  2. prefix property: random tables (few distinct keys, NULLs, two
 *	     keys, mixed directions), random K and OFFSET, more rows than K
 *	     so the bounded buffer evicts: LIMIT K OFFSET O equals rows
 *	     O..O+K-1 of the same query without LIMIT.  Seeded, so a
 *	     failure prints a reproducible case.
 *	  3. input orders that drive the eviction path hardest: ascending,
 *	     descending and all-equal keys, against the unlimited query.
 *	  4. memory: ORDER BY x DESC LIMIT 10 over rows arriving ascending
 *	     (every row beats the kept ones) with a 4 KB TEXT per row must
 *	     not hold every admitted row's bytes until the statement ends:
 *	     peak RSS growth during the query stays far below the ~100 MB
 *	     the 24,000 admitted rows' text would take (Linux: resets
 *	     VmHWM through /proc/self/clear_refs; skipped elsewhere, and
 *	     under ASan/TSan, whose allocators keep freed blocks resident).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

static const char *setup[] = {
	"CREATE TABLE t(k INTEGER, j INTEGER, v TEXT)",
	"INSERT INTO t VALUES(5,1,'a'),(3,2,'b'),(5,1,'c'),(NULL,3,'d'),"
	"(3,1,'e'),(5,2,'f'),(NULL,1,'g'),(1,9,'h'),(5,1,'i'),(3,2,'j')",
	NULL
};

static const struct {
	const char *sql, *want;
} shapes[] = {
	{ "SELECT k, v FROM t ORDER BY k LIMIT 4", "NULL|d;NULL|g;1|h;3|b" },
	{ "SELECT k, v FROM t ORDER BY k",
	  "NULL|d;NULL|g;1|h;3|b;3|e;3|j;5|a;5|c;5|f;5|i" },
	{ "SELECT k, v FROM t ORDER BY k DESC LIMIT 4", "5|a;5|c;5|f;5|i" },
	{ "SELECT k, v FROM t ORDER BY k DESC",
	  "5|a;5|c;5|f;5|i;3|b;3|e;3|j;1|h;NULL|d;NULL|g" },
	{ "SELECT k, j, v FROM t ORDER BY k, j LIMIT 5",
	  "NULL|1|g;NULL|3|d;1|9|h;3|1|e;3|2|b" },
	{ "SELECT k, j, v FROM t ORDER BY k DESC, j LIMIT 5",
	  "5|1|a;5|1|c;5|1|i;5|2|f;3|1|e" },
	{ "SELECT k, j, v FROM t ORDER BY k DESC, j",
	  "5|1|a;5|1|c;5|1|i;5|2|f;3|1|e;3|2|b;3|2|j;1|9|h;NULL|1|g;NULL|3|d" },
	{ "SELECT k, v FROM t ORDER BY k NULLS LAST LIMIT 4", "1|h;3|b;3|e;3|j" },
	{ "SELECT k, v FROM t ORDER BY k DESC NULLS FIRST LIMIT 4",
	  "NULL|d;NULL|g;5|a;5|c" },
	{ "SELECT k, v FROM t ORDER BY k DESC NULLS FIRST",
	  "NULL|d;NULL|g;5|a;5|c;5|f;5|i;3|b;3|e;3|j;1|h" },
	{ "SELECT DISTINCT k FROM t ORDER BY k LIMIT 3", "NULL;1;3" },
	{ "SELECT DISTINCT k, j FROM t ORDER BY k DESC LIMIT 3", "5|1;5|2;3|2" },
	{ "SELECT k, v FROM t ORDER BY k LIMIT 3 OFFSET 2", "1|h;3|b;3|e" },
	{ "SELECT k, v FROM t ORDER BY k DESC LIMIT 3 OFFSET 3", "5|i;3|b;3|e" },
	{ "SELECT k, v FROM t ORDER BY k LIMIT 0", "" },
	{ "SELECT k, v FROM t ORDER BY k % 2, k LIMIT 5",
	  "NULL|d;NULL|g;1|h;3|b;3|e" },
	{ "SELECT k, v FROM t ORDER BY j LIMIT 4", "5|a;5|c;3|e;NULL|g" },
	{ "SELECT k, v FROM t ORDER BY k LIMIT 100",
	  "NULL|d;NULL|g;1|h;3|b;3|e;3|j;5|a;5|c;5|f;5|i" },
	{ "SELECT k, v FROM t WHERE j < 3 ORDER BY k DESC LIMIT 3",
	  "5|a;5|c;5|f" },
};

struct buf {
	char  *p;
	size_t n, cap;
	int    nrow;
};

static void
buf_put(struct buf *b, const char *s)
{
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		char *np;
		while (nc < b->n + l + 1) nc *= 2;
		if ((np = realloc(b->p, nc)) == NULL) abort();
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->n, s, l + 1);
	b->n += l;
}

/* Rows rendered "c1|c2" joined by ';' in the order returned.  -1 on an
 * error from prepare or step. */
static int
render(sx_db *db, const char *sql, struct buf *out)
{
	sx_stmt *st = NULL;
	int s, c;
	char cell[64];

	out->n = 0;
	out->nrow = 0;
	buf_put(out, "");
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK || st == NULL)
		return -1;
	while ((s = sx_step(st)) == SX_ROW) {
		if (out->nrow++) buf_put(out, ";");
		for (c = 0; c < sx_column_count(st); c++) {
			if (c) buf_put(out, "|");
			if (sx_column_type(st, c) == SX_NULL)
				snprintf(cell, sizeof cell, "NULL");
			else if (sx_column_type(st, c) == SX_INTEGER)
				snprintf(cell, sizeof cell, "%lld",
				    (long long)sx_column_int64(st, c));
			else
				snprintf(cell, sizeof cell, "%s",
				    sx_column_text(st, c) ? sx_column_text(st, c) : "");
			buf_put(out, cell);
		}
	}
	sx_finalize(st);
	return s == SX_DONE ? 0 : -1;
}

/* The [off, off + lim) rows of a rendered result, re-joined. */
static void
slice(const char *all, int off, int lim, struct buf *out)
{
	const char *p = all;
	int r = 0;

	out->n = 0;
	out->nrow = 0;
	buf_put(out, "");
	while (*p != '\0' && r < off + lim) {
		const char *e = strchr(p, ';');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (r >= off) {
			char row[256];
			if (l >= sizeof row) l = sizeof row - 1;
			memcpy(row, p, l);
			row[l] = '\0';
			if (out->nrow++) buf_put(out, ";");
			buf_put(out, row);
		}
		r++;
		p = e ? e + 1 : p + l;
	}
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define T_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define T_SANITIZER 1
#endif
#endif

/* Reset VmHWM (clear_refs "5"): 0, or -1 if the platform cannot. */
static long
hwm_reset(void)
{
	FILE *f;
#ifdef T_SANITIZER
	return -1;   /* quarantined frees stay resident: RSS says nothing */
#endif
	f = fopen("/proc/self/clear_refs", "w");
	if (f == NULL) return -1;
	if (fputs("5", f) < 0) { fclose(f); return -1; }
	fclose(f);
	return 0;
}

static long
hwm_kb(void)
{
	char line[128];
	long v = -1;
	FILE *f = fopen("/proc/self/status", "r");
	if (f == NULL) return -1;
	while (fgets(line, sizeof line, f) != NULL)
		if (sscanf(line, "VmHWM: %ld", &v) == 1) break;
	fclose(f);
	return v;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t
rnd(uint32_t n)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return (uint32_t)(g_rng % n);
}

/* Compare LIMIT `lim` OFFSET `off` of `base` (an ORDER BY query) with
 * the same slice of the unlimited result `all`.  Returns 1 if equal. */
static int
check_prefix(sx_db *db, const char *base, const char *all, int lim, int off,
    struct buf *got, struct buf *want)
{
	char sql[512];

	snprintf(sql, sizeof sql, "%s LIMIT %d OFFSET %d", base, lim, off);
	slice(all, off, lim, want);
	if (render(db, sql, got) != 0 || strcmp(got->p, want->p) != 0) {
		printf("  FAIL  %s\n        got  %.300s\n        want %.300s\n", sql,
		    got->p, want->p);
		return 0;
	}
	return 1;
}

int
main(void)
{
	char path[256], sql[512];
	const char *sfx[] = { "-wal", ".dwb" };
	sx_db *db = NULL;
	struct buf got = { 0 }, want = { 0 }, all = { 0 };
	int fd, i, nok = 0, nfail = 0, round, off;
	long before = 0, after = 0;
	static char bigq[4400];
	static const char *const dir[] = { "", " DESC", " NULLS LAST",
	    " DESC NULLS FIRST" };

	t_tmpl(path, sizeof path, "sqlxtc-ordlim");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 2048) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	for (i = 0; setup[i] != NULL; i++)
		CK(sx_exec(db, setup[i], NULL) == SX_OK);
	if (g_fail) return 1;

	/* 1. SQLite's answers, compared in order. */
	for (i = 0; i < (int)(sizeof shapes / sizeof shapes[0]); i++) {
		if (render(db, shapes[i].sql, &got) == 0 &&
		    strcmp(got.p, shapes[i].want) == 0) {
			nok++;
			continue;
		}
		printf("  FAIL  %s\n        got  %s\n        want %s\n", shapes[i].sql,
		    got.p, shapes[i].want);
		nfail++;
	}

	/* 2. Prefix property over random tie-heavy tables. */
	for (round = 0; round < 12; round++) {
		int n = 50 + (int)rnd(400), dom = 1 + (int)rnd(12), q;
		CK(sx_exec(db, "DROP TABLE IF EXISTS r", NULL) == SX_OK);
		CK(sx_exec(db, "CREATE TABLE r(a INTEGER, b INTEGER, s TEXT)", NULL)
		    == SX_OK);
		CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
		for (i = 0; i < n; i++) {
			uint32_t a = rnd((uint32_t)dom + 1);
			if (a == (uint32_t)dom)
				snprintf(sql, sizeof sql,
				    "INSERT INTO r VALUES(NULL, %u, 'r%d')", rnd(3), i);
			else
				snprintf(sql, sizeof sql,
				    "INSERT INTO r VALUES(%u, %u, 'r%d')", a, rnd(3), i);
			CK(sx_exec(db, sql, NULL) == SX_OK);
		}
		CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
		for (q = 0; q < 8; q++) {
			char base[256];
			int t;
			if (q & 1)
				snprintf(base, sizeof base,
				    "SELECT a, b, s FROM r ORDER BY a%s, b%s",
				    dir[rnd(4)], dir[rnd(2)]);
			else
				snprintf(base, sizeof base,
				    "SELECT a, s FROM r ORDER BY a%s", dir[rnd(4)]);
			if (render(db, base, &all) != 0 || all.nrow != n) {
				printf("  FAIL  %s: %d rows, want %d\n", base, all.nrow, n);
				nfail++;
				continue;
			}
			for (t = 0; t < 6; t++) {
				int lim = 1 + (int)rnd((uint32_t)n / 2), off = (t & 1) ?
				    (int)rnd((uint32_t)n / 4) : 0;
				if (check_prefix(db, base, all.p, lim, off, &got, &want))
					nok++;
				else
					nfail++;
			}
		}
	}

	/* 3. Ascending / descending / all-equal arrival: every row of the
	 * descending input replaces the worst kept one. */
	CK(sx_exec(db, "CREATE TABLE m(id INTEGER PRIMARY KEY, up INTEGER, "
	    "dn INTEGER, eq INTEGER)", NULL) == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 1; i <= 3000; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO m VALUES(%d, %d, %d, 7)", i, i,
		    3001 - i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	for (i = 0; i < 6; i++) {
		static const char *const ord[] = { "up", "up DESC", "dn", "dn DESC",
		    "eq", "eq DESC" };
		static const int lims[] = { 1, 10, 999 };
		char base[128];
		int l;
		snprintf(base, sizeof base, "SELECT id FROM m ORDER BY %s", ord[i]);
		if (render(db, base, &all) != 0 || all.nrow != 3000) {
			printf("  FAIL  %s\n", base);
			nfail++;
			continue;
		}
		for (l = 0; l < 3; l++) {
			if (check_prefix(db, base, all.p, lims[l], l == 2 ? 17 : 0, &got,
			    &want))
				nok++;
			else
				nfail++;
		}
	}

	/* 4. Bytes of rows the cut drops are released during the scan. */
	CK(sx_exec(db, "CREATE TABLE big(id INTEGER PRIMARY KEY, up INTEGER)", NULL)
	    == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 1; i <= 24000; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO big VALUES(%d, %d)", i, i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	off = snprintf(bigq, sizeof bigq, "SELECT id, id || '");
	memset(bigq + off, 'z', 4096);
	snprintf(bigq + off + 4096, sizeof bigq - (size_t)off - 4096,
	    "' FROM big ORDER BY up DESC LIMIT 10");
	if (render(db, "SELECT count(*) FROM big", &got) == 0 &&
	    hwm_reset() == 0 && (before = hwm_kb()) > 0) {
		int r = render(db, bigq, &got);
		after = hwm_kb();
		printf("  top-10 of 24000 rows x 4 KB text: peak RSS +%ld KB\n",
		    after - before);
		if (r == 0 && got.nrow == 10 && after - before < 32 * 1024)
			nok++;
		else {
			printf("  FAIL  top-N held %ld KB of dropped rows' bytes "
			    "(bound 32768 KB), rows %d\n", after - before, got.nrow);
			nfail++;
		}
	} else
		printf("  skip  memory check (no /proc/self/clear_refs, or a "
		    "sanitizer build)\n");

	sx_close(db);
	CK(sx_storage_close() == SX_OK);
	unlink(path);
	for (i = 0; i < 2; i++) {
		snprintf(sql, sizeof sql, "%s%s", path, sfx[i]);
		unlink(sql);
	}
	free(got.p);
	free(want.p);
	free(all.p);
	printf("test_order_limit: %d ok, %d FAIL\n", nok, nfail);
	return (nfail || g_fail) ? 1 : 0;
}
