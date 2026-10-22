/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_idx_elem.c
 *	The index key element of a number (xstore_index_elem, xstore.c
 *	xs_tup_num).
 *
 *	(1) Order: for random pairs of numbers -- integers at every byte
 *	    width edge, INT64_MIN/MAX, REALs with and without a fraction,
 *	    +-2^53 +- 1, +-2^63, huge and tiny REALs, +-inf, -0.0 -- memcmp
 *	    of the two elements has the sign of SQLite's comparison of the
 *	    values (an INTEGER and a REAL compare by exact value, so 3 and
 *	    3.0 are equal and INT64_MAX < 2^63), ascending and DESC; equal
 *	    values give identical elements; and no element is a proper
 *	    prefix of another (a scan matches a bound as a byte prefix).
 *	(2) Size: an integer below 64 is 2 bytes, below 2^15 3 bytes, a
 *	    microsecond timestamp 8 bytes (every number was 11 bytes).
 *	(3) SQL: a UNIQUE index accepts both INT64_MAX and the REAL 2^63
 *	    (they are different values; the old encoding gave them one key
 *	    and refused the second row), and for a column of mixed numbers
 *	    every equality and range query through an index returns the
 *	    rows a full scan returns, ascending and DESC.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "t_tmp.h"

static uint64_t g_s = 0x9E3779B97F4A7C15ull;
static uint64_t
rnd(void)
{
	g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17;
	return g_s;
}

static const int64_t edge_i[] = {
	0, 1, -1, 2, 63, 64, 127, 128, 255, 256, 32767, 32768, 65535, 65536,
	-128, -129, -255, -256, -32768, -32769, 8388607, 8388608,
	2147483647LL, 2147483648LL, -2147483648LL, -2147483649LL,
	9007199254740991LL, 9007199254740992LL, 9007199254740993LL,
	-9007199254740992LL, -9007199254740993LL,
	4611686018427387903LL, 4611686018427387904LL,
	INT64_MAX, INT64_MAX - 1, INT64_MIN, INT64_MIN + 1
};
static const double edge_r[] = {
	0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5, 63.0, 63.5, 64.0, 127.75,
	1e-300, -1e-300, 4.9e-324, 3.25, -3.25, 1234567.875,
	9007199254740992.0, 9007199254740994.0, -9007199254740992.0,
	4611686018427387904.0, 9223372036854774784.0, 9223372036854775808.0,
	-9223372036854775808.0, 18446744073709551616.0, -18446744073709551616.0,
	1e300, -1e300
};

struct num { int is_int; int64_t i; double r; };

static void
gen(struct num *n)
{
	int k = (int)(rnd() % 6);
	memset(n, 0, sizeof *n);
	switch (k) {
	case 0: n->is_int = 1; n->i = edge_i[rnd() % (sizeof edge_i / sizeof edge_i[0])]; break;
	case 1: n->is_int = 1; n->i = (int64_t)rnd() >> (rnd() % 64); break;
	case 2: n->r = edge_r[rnd() % (sizeof edge_r / sizeof edge_r[0])];
		if (rnd() % 8 == 0) n->r = (rnd() & 1) ? INFINITY : -INFINITY;
		break;
	case 3: n->r = (double)((int64_t)rnd() >> (rnd() % 64)) / (double)(1u << (rnd() % 12)); break;
	case 4: n->r = (double)edge_i[rnd() % (sizeof edge_i / sizeof edge_i[0])]; break;
	default: n->is_int = 1; n->i = (int64_t)(rnd() % 200) - 100; break;
	}
}

/* SQLite's numeric comparison (exact across INTEGER and REAL): an x86-64
 * long double holds every int64 and every double exactly. */
static int
ref_cmp(const struct num *a, const struct num *b)
{
	long double x = a->is_int ? (long double)a->i : (long double)a->r;
	long double y = b->is_int ? (long double)b->i : (long double)b->r;
	return x < y ? -1 : x > y ? 1 : 0;
}

static int
elem(const struct num *n, uint8_t *out, int desc)
{
	xstore_val_t v;
	memset(&v, 0, sizeof v);
	v.cls = n->is_int ? XSTORE_C_INT : XSTORE_C_REAL;
	v.i = n->i;
	v.r = n->r;
	return xstore_index_elem(out, 64, &v, desc);
}

static int
sgn(int x)
{
	return x < 0 ? -1 : x > 0 ? 1 : 0;
}

static void
t_order(void)
{
	struct num a, b;
	uint8_t ea[64], eb[64];
	int t, la, lb, c, want, desc, bad = 0, lim;

	for (t = 0; t < 2000000; t++) {
		gen(&a);
		gen(&b);
		desc = (int)(rnd() & 1);
		la = elem(&a, ea, desc);
		lb = elem(&b, eb, desc);
		if (la <= 0 || lb <= 0) { bad++; continue; }
		lim = la < lb ? la : lb;
		c = memcmp(ea, eb, (size_t)lim);
		want = ref_cmp(&a, &b) * (desc ? -1 : 1);
		if (want == 0 ? (la != lb || c != 0) : (c == 0 || sgn(c) != want)) {
			/* c == 0 with unequal values: one is a prefix of the other */
			if (bad++ < 5)
				fprintf(stderr, "  %s %lld/%.17g vs %s %lld/%.17g desc %d: "
				    "memcmp %d (len %d/%d), want %d\n",
				    a.is_int ? "int" : "real", (long long)a.i, a.r,
				    b.is_int ? "int" : "real", (long long)b.i, b.r, desc,
				    c, la, lb, want);
		}
	}
	if (bad) { fprintf(stderr, "FAIL order: %d bad pairs\n", bad); g_fail = 1; }
	else printf("  ok   2000000 random number pairs: memcmp order == SQLite order, "
	    "no element a prefix of another (asc + desc)\n");
}

static void
t_size(void)
{
	struct num n;
	uint8_t e[64];
	int l7, l1000, lts, lr;

	memset(&n, 0, sizeof n);
	n.is_int = 1;
	n.i = 7; l7 = elem(&n, e, 0);
	n.i = 1000; l1000 = elem(&n, e, 0);
	n.i = 1761062400000000LL; lts = elem(&n, e, 0);   /* microseconds, 2025 */
	n.is_int = 0; n.r = 12.5; lr = elem(&n, e, 0);
	printf("  element bytes: 7 -> %d, 1000 -> %d, a us timestamp -> %d, 12.5 -> %d\n",
	    l7, l1000, lts, lr);
	if (l7 != 2 || l1000 != 3 || lts != 8) {
		fprintf(stderr, "FAIL size: want 2 / 3 / 8 bytes\n");
		g_fail = 1;
	}
}

static int64_t
q1(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* A predicate on v with up to two bound numbers. */
struct pred { const char *where; int nb; struct num b[2]; };

static void
bind_num(sx_stmt *st, int i, const struct num *n)
{
	if (n->is_int) sx_bind_int64(st, i, n->i);
	else sx_bind_double(st, i, n->r);
}

/* count(*) and a rowid checksum of `SELECT .. FROM tab WHERE pred`. */
static void
run_pred(sx_db *db, const char *tab, const struct pred *p, int64_t *cnt,
    double *sum)
{
	char sql[256];
	sx_stmt *st = NULL;
	int j;
	*cnt = -1; *sum = -1;
	snprintf(sql, sizeof sql, "SELECT count(*), total(k * k) FROM %s WHERE %s",
	    tab, p->where);
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return;
	for (j = 0; j < p->nb; j++) bind_num(st, j + 1, &p->b[j]);
	if (sx_step(st) == SX_ROW) {
		*cnt = sx_column_int64(st, 0);
		*sum = sx_column_double(st, 1);
	}
	sx_finalize(st);
}

/* A stored value of the test column: 0 NULL, 1 number, 2 TEXT. */
struct cell { int kind; struct num n; };

/* SQLite's comparison of a stored value with a bound number (TEXT sorts
 * after every number; NULL matches nothing, the caller's job). */
static int
cell_cmp(const struct cell *c, const struct num *b)
{
	return c->kind == 2 ? 1 : ref_cmp(&c->n, b);
}

static int
pred_true(const struct pred *p, const struct cell *c)
{
	int x = c->kind == 0 ? 0 : cell_cmp(c, &p->b[0]);
	if (c->kind == 0) return 0;
	if (strcmp(p->where, "v = ?") == 0) return x == 0;
	if (strcmp(p->where, "v > ?") == 0) return x > 0;
	if (strcmp(p->where, "v >= ?") == 0) return x >= 0;
	if (strcmp(p->where, "v < ?") == 0) return x < 0;
	if (strcmp(p->where, "v <= ?") == 0) return x <= 0;
	if (strcmp(p->where, "v > ? AND v < ?") == 0)
		return x > 0 && cell_cmp(c, &p->b[1]) < 0;
	return x >= 0 && cell_cmp(c, &p->b[1]) <= 0;   /* BETWEEN */
}

/* The index seek of table t returns exactly the rows SQLite's comparison
 * selects (count and rowid checksum), and it did seek. */
static int
index_right(sx_db *db, const struct pred *p, const struct cell *cells, int n)
{
	int64_t ca, cb = 0;
	double sa, sb = 0;
	uint64_t s0, s1;
	int k;
	s0 = xstore_index_seek_count();
	run_pred(db, "t", p, &ca, &sa);
	s1 = xstore_index_seek_count();
	for (k = 1; k <= n; k++)
		if (pred_true(p, &cells[k])) { cb++; sb += (double)k * k; }
	if (s1 == s0 || ca != cb || sa != sb) {
		fprintf(stderr, "  WHERE %s [%s %lld/%.17g, %s %lld/%.17g]: index %lld rows "
		    "(sum %.17g, %llu seeks), want %lld (sum %.17g)\n", p->where,
		    p->b[0].is_int ? "int" : "real", (long long)p->b[0].i, p->b[0].r,
		    p->b[1].is_int ? "int" : "real", (long long)p->b[1].i, p->b[1].r,
		    (long long)ca, sa, (unsigned long long)(s1 - s0), (long long)cb, sb);
		return 0;
	}
	return 1;
}

/* A bound for the SQL check: a number whose comparison with every stored
 * value is exact in double arithmetic as well (|v| <= 2^53 or a REAL). */
static void
gen_bound(struct num *n)
{
	do {
		gen(n);
	} while (n->is_int && (n->i > (1LL << 53) || n->i < -(1LL << 53)));
}

static void
t_sql(void)
{
	/* Stored values.  Not INT64_MIN, nor integers beyond 2^53 beside an
	 * equal-rounding REAL: the executor's own comparison (vexec.c
	 * cmp_vals) is not exact there (see REQUESTS in the SIZE report), so
	 * the reference would disagree with SQLite; the encoder's order at
	 * those edges is checked exactly by t_order. */
	static const char *vals[] = {
		"0", "0.0", "-0.0", "1", "1.0", "1.5", "-1", "-1.5", "63", "64", "64.25",
		"255", "256", "-256", "-255.5", "32767", "32768", "1e15",
		"1000000000000000.5", "9007199254740992", "9007199254740992.0",
		"4503599627370497", "-4503599627370497", "4503599627370496.5",
		"1e300", "-1e300", "4.9e-324", "-4.9e-324", "1e20", "-1e20",
		"123456789012", "123456789012.75", "-123456789012.75", "NULL", "'x'"
	};
	static const char *ops[] = { "v = ?", "v > ?", "v >= ?", "v < ?",
	    "v <= ?", "v > ? AND v < ?", "v BETWEEN ? AND ?" };
	struct pred p;
	static struct cell cells[1 + 3 * 64];
	char path[256], sql[300];
	sx_db *db = NULL;
	int i, k = 0, rep, bad = 0, rc, n = 0;

	t_tmpl(path, sizeof path, "sqlxtc-idxelem");
	{ int fd = mkstemp(path); if (fd >= 0) close(fd); }
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL) return;

	/* UNIQUE: INT64_MAX and the REAL 2^63 are different values. */
	CK(sx_exec(db, "CREATE TABLE u(k INTEGER PRIMARY KEY, a)", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE UNIQUE INDEX u_a ON u(a)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO u VALUES(1, 9223372036854775807)", NULL) == SX_OK);
	rc = sx_exec(db, "INSERT INTO u VALUES(2, 9223372036854775808.0)", NULL);
	if (rc != SX_OK) {
		fprintf(stderr, "FAIL unique: the REAL 2^63 conflicts with INT64_MAX (rc %d)\n", rc);
		g_fail = 1;
	}
	CK(sx_exec(db, "INSERT INTO u VALUES(3, 9223372036854775807)", NULL) != SX_OK);
	CK(sx_exec(db, "INSERT INTO u VALUES(4, 3)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO u VALUES(5, 3.0)", NULL) != SX_OK);   /* 3 == 3.0 */
	CK(q1(db, "SELECT count(*) FROM u") == 3);

	/* An index over a column of mixed numbers (plus NULL and TEXT). */
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	k = 0;
	for (rep = 0; rep < 3; rep++)
		for (i = 0; i < (int)(sizeof vals / sizeof vals[0]); i++) {
			struct cell *c = &cells[++k];
			snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, %s)", k, vals[i]);
			CK(sx_exec(db, sql, NULL) == SX_OK);
			memset(c, 0, sizeof *c);
			if (strcmp(vals[i], "NULL") == 0) {
				c->kind = 0;
			} else if (vals[i][0] == '\'') {
				c->kind = 2;
			} else if (strpbrk(vals[i], ".e") != NULL) {
				c->kind = 1; c->n.r = strtod(vals[i], NULL);
			} else {
				c->kind = 1; c->n.is_int = 1;
				c->n.i = strtoll(vals[i], NULL, 10);
			}
		}
	CK(sx_exec(db, "CREATE INDEX t_v ON t(v)", NULL) == SX_OK);
	for (i = 0; i < 3000; i++) {
		memset(&p, 0, sizeof p);
		p.where = ops[rnd() % (sizeof ops / sizeof ops[0])];
		p.nb = strchr(strchr(p.where, '?') + 1, '?') != NULL ? 2 : 1;
		gen_bound(&p.b[0]);
		gen_bound(&p.b[1]);
		n++;
		if (!index_right(db, &p, cells, k) && bad++ > 8) break;
	}
	CK(sx_exec(db, "DROP INDEX t_v", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE INDEX t_vd ON t(v DESC)", NULL) == SX_OK);
	for (i = 0; i < 3000 && bad <= 8; i++) {
		memset(&p, 0, sizeof p);
		p.where = ops[0];     /* a DESC column seeks equality only */
		p.nb = 1;
		gen_bound(&p.b[0]);
		n++;
		if (!index_right(db, &p, cells, k)) bad++;
	}
	if (bad) { fprintf(stderr, "FAIL sql: %d of %d predicates differ\n", bad, n); g_fail = 1; }
	else printf("  ok   UNIQUE keeps INT64_MAX and 2^63 apart; %d random bound "
	    "predicates: the index seek returns exactly SQLite's rows (asc + desc)\n", n);
	sx_close(db);
	(void)sx_storage_close();
	unlink(path);
	snprintf(sql, sizeof sql, "%s-wal", path);
	unlink(sql);
}

int
main(void)
{
	printf("test_idx_elem\n");
	t_order();
	t_size();
	t_sql();
	if (g_fail) { fprintf(stderr, "FAIL: test_idx_elem\n"); return 1; }
	printf("  ok   index elements\n");
	return 0;
}
