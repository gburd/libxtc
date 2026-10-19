/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_tpcc_seek.c
 *	Seek, not scan, for the TPROC-C statement shapes: every one of the
 *	35 statements the Turso C harness prepares (turso main.c:662-886),
 *	on Turso's own schema (composite PRIMARY KEYs + its 4 CREATE
 *	INDEX), driven with BOUND ? parameters, must visit about as many
 *	base rows as SQLite would (xstore_rows_visited), not O(table).  The
 *	tables hold far more rows than any bound below, so a full scan
 *	fails loudly.  Also asserted: an ORDER BY the index provides runs
 *	no sort (vx_sort_count), and MIN/MAX over an index prefix reads one
 *	row.  No daemon; off a loop (synchronous bufmgr I/O).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "vexec.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_W   2         /* warehouses */
#define N_D   10        /* districts per warehouse */
#define N_C   30        /* customers per district */
#define N_O   30        /* orders per district */
#define N_I   1000      /* items */

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static const char *const SCHEMA[] = {
	"create table warehouse (w_id integer not null, w_name text, "
	"w_street_1 text, w_street_2 text, w_city text, w_state text, "
	"w_zip text, w_tax real, w_ytd real, primary key (w_id))",
	"create table district (d_id integer not null, d_w_id integer not null, "
	"d_name text, d_street_1 text, d_street_2 text, d_city text, "
	"d_state text, d_zip text, d_tax real, d_ytd real, d_next_o_id integer, "
	"primary key (d_w_id, d_id))",
	"create table customer (c_id integer not null, c_d_id integer not null, "
	"c_w_id integer not null, c_first text, c_middle text, c_last text, "
	"c_street_1 text, c_street_2 text, c_city text, c_state text, "
	"c_zip text, c_phone text, c_since text, c_credit text, "
	"c_credit_lim integer, c_discount real, c_balance real, "
	"c_ytd_payment real, c_payment_cnt integer, c_delivery_cnt integer, "
	"c_data text, primary key(c_w_id, c_d_id, c_id))",
	"create table history (h_c_id integer, h_c_d_id integer, "
	"h_c_w_id integer, h_d_id integer, h_w_id integer, h_date text, "
	"h_amount real, h_data text)",
	"create table new_orders (no_o_id integer not null, "
	"no_d_id integer not null, no_w_id integer not null, "
	"primary key(no_w_id, no_d_id, no_o_id))",
	"create table orders (o_id integer not null, o_d_id integer not null, "
	"o_w_id integer not null, o_c_id integer, o_entry_d text, "
	"o_carrier_id integer, o_ol_cnt integer, o_all_local integer, "
	"primary key(o_w_id, o_d_id, o_id))",
	"create table order_line (ol_o_id integer not null, "
	"ol_d_id integer not null, ol_w_id integer not null, "
	"ol_number integer not null, ol_i_id integer, ol_supply_w_id integer, "
	"ol_delivery_d text, ol_quantity integer, ol_amount real, "
	"ol_dist_info text, primary key(ol_w_id, ol_d_id, ol_o_id, ol_number))",
	"create table item (i_id integer not null, i_im_id integer, "
	"i_name text, i_price real, i_data text, primary key(i_id))",
	"create table stock (s_i_id integer not null, s_w_id integer not null, "
	"s_quantity integer, s_dist_01 text, s_dist_02 text, s_dist_03 text, "
	"s_dist_04 text, s_dist_05 text, s_dist_06 text, s_dist_07 text, "
	"s_dist_08 text, s_dist_09 text, s_dist_10 text, s_ytd real, "
	"s_order_cnt integer, s_remote_cnt integer, s_data text, "
	"primary key(s_w_id, s_i_id))",
	"CREATE INDEX IF NOT EXISTS idx_customer ON customer "
	"(c_w_id,c_d_id,c_last,c_first)",
	"CREATE INDEX IF NOT EXISTS idx_orders ON orders (o_w_id,o_d_id,o_c_id,o_id)",
	"CREATE INDEX IF NOT EXISTS fkey_stock_2 ON stock (s_i_id)",
	"CREATE INDEX IF NOT EXISTS fkey_order_line_2 ON order_line "
	"(ol_supply_w_id,ol_i_id)",
};

/* The 35 Turso statements, verbatim. */
static const char *const T[35] = {
	"SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse "
	"WHERE w_id = ? AND c_w_id = w_id AND c_d_id = ? AND c_id = ?",
	"SELECT d_next_o_id, d_tax FROM district WHERE d_id = ? AND d_w_id = ?",
	"UPDATE district SET d_next_o_id = ? + 1 WHERE d_id = ? AND d_w_id = ?",
	"INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, "
	"o_ol_cnt, o_all_local) VALUES(?, ?, ?, ?, ?, ?, ?)",
	"INSERT INTO new_orders (no_o_id, no_d_id, no_w_id) VALUES (?,?,?)",
	"SELECT i_price, i_name, i_data FROM item WHERE i_id = ?",
	"SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, "
	"s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, "
	"s_dist_10 FROM stock WHERE s_i_id = ? AND s_w_id = ?",
	"UPDATE stock SET s_quantity = ? WHERE s_i_id = ? AND s_w_id = ?",
	"INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, "
	"ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) "
	"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
	"UPDATE warehouse SET w_ytd = w_ytd + ? WHERE w_id = ?",
	"SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name "
	"FROM warehouse WHERE w_id = ?",
	"UPDATE district SET d_ytd = d_ytd + ? WHERE d_w_id = ? AND d_id = ?",
	"SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM "
	"district WHERE d_w_id = ? AND d_id = ?",
	"SELECT count(c_id) FROM customer WHERE c_w_id = ? AND c_d_id = ? "
	"AND c_last = ?",
	"SELECT c_id FROM customer WHERE c_w_id = ? AND c_d_id = ? AND "
	"c_last = ? ORDER BY c_first",
	"SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, "
	"c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, "
	"c_balance, c_since FROM customer WHERE c_w_id = ? AND c_d_id = ? "
	"AND c_id = ?",
	"SELECT c_data FROM customer WHERE c_w_id = ? AND c_d_id = ? AND "
	"c_id = ?",
	"UPDATE customer SET c_balance = ?, c_data = ? WHERE c_w_id = ? AND "
	"c_d_id = ? AND c_id = ?",
	"UPDATE customer SET c_balance = ? WHERE c_w_id = ? AND c_d_id = ? "
	"AND c_id = ?",
	"INSERT INTO history(h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, "
	"h_date, h_amount, h_data) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
	"SELECT count(c_id) FROM customer WHERE c_w_id = ? AND c_d_id = ? "
	"AND c_last = ?",
	"SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE "
	"c_w_id = ? AND c_d_id = ? AND c_last = ? ORDER BY c_first",
	"SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE "
	"c_w_id = ? AND c_d_id = ? AND c_id = ?",
	"SELECT o_id, o_entry_d, COALESCE(o_carrier_id,0) FROM orders WHERE "
	"o_w_id = ? AND o_d_id = ? AND o_c_id = ? AND o_id = (SELECT "
	"MAX(o_id) FROM orders WHERE o_w_id = ? AND o_d_id = ? AND o_c_id = ?)",
	"SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, "
	"ol_delivery_d FROM order_line WHERE ol_w_id = ? AND ol_d_id = ? AND "
	"ol_o_id = ?",
	"SELECT COALESCE(MIN(no_o_id),0) FROM new_orders WHERE no_d_id = ? "
	"AND no_w_id = ?",
	"DELETE FROM new_orders WHERE no_o_id = ? AND no_d_id = ? AND "
	"no_w_id = ?",
	"SELECT o_c_id FROM orders WHERE o_id = ? AND o_d_id = ? AND o_w_id = ?",
	"UPDATE orders SET o_carrier_id = ? WHERE o_id = ? AND o_d_id = ? "
	"AND o_w_id = ?",
	"UPDATE order_line SET ol_delivery_d = ? WHERE ol_o_id = ? AND "
	"ol_d_id = ? AND ol_w_id = ?",
	"SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = ? AND "
	"ol_d_id = ? AND ol_w_id = ?",
	"UPDATE customer SET c_balance = c_balance + ? , c_delivery_cnt = "
	"c_delivery_cnt + 1 WHERE c_id = ? AND c_d_id = ? AND c_w_id = ?",
	"SELECT d_next_o_id FROM district WHERE d_id = ? AND d_w_id = ?",
	"SELECT DISTINCT ol_i_id FROM order_line WHERE ol_w_id = ? AND "
	"ol_d_id = ? AND ol_o_id < ? AND ol_o_id >= (? - 20)",
	"SELECT count(*) FROM stock WHERE s_w_id = ? AND s_i_id = ? AND "
	"s_quantity < ?",
};

struct prm { char t; int64_t i; double r; const char *s; };
#define PI(x) { 'i', (x), 0, NULL }
#define PR(x) { 'r', 0, (x), NULL }
#define PS(x) { 's', 0, 0, (x) }

static const char *const LASTS[4] = {
	"BARBARBAR", "BAROUGHTABLE", "ABLEPRIPRES", "OUGHTESEANTI"
};

static int
bind_all(sx_stmt *st, const struct prm *p, int np)
{
	int i, rc = SX_OK;
	for (i = 0; i < np && rc == SX_OK; i++)
		switch (p[i].t) {
		case 'i': rc = sx_bind_int64(st, i + 1, p[i].i); break;
		case 'r': rc = sx_bind_double(st, i + 1, p[i].r); break;
		case 's': rc = sx_bind_text(st, i + 1, p[i].s, -1); break;
		default:  rc = sx_bind_null(st, i + 1); break;
		}
	return rc;
}

/* Run `sql` once with binds; return the row count (-1 on error), the
 * first column of the first row in *first, rows visited, sorts run. */
static int64_t g_chg;   /* sx_stmt_changes of the last run() */

static int
run(sx_db *db, const char *sql, const struct prm *p, int np, int64_t *first,
    uint64_t *visited, uint64_t *sorts)
{
	sx_stmt *st = NULL;
	uint64_t v0, s0;
	int n = 0, rc;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	if (bind_all(st, p, np) != SX_OK) { sx_finalize(st); return -1; }
	v0 = xstore_rows_visited();
	s0 = vx_sort_count();
	while ((rc = sx_step(st)) == SX_ROW) {
		if (n == 0 && first) *first = sx_column_int64(st, 0);
		n++;
	}
	if (visited) *visited = xstore_rows_visited() - v0;
	if (sorts) *sorts = vx_sort_count() - s0;
	g_chg = sx_stmt_changes(st);
	sx_finalize(st);
	if (rc != SX_DONE)
		fprintf(stderr, "  error: %s\n", sx_errmsg(db));
	return rc == SX_DONE ? n : -1;
}

/* One statement case: want_rows < 0 = any (a write: then want_chg is
 * the sx_changes count), max_vis = the visited bound (the rows SQLite's
 * plan reads), want_first =
 * INT64_MIN to skip the value check, nosort = an ORDER BY the chosen
 * index provides (no sort may run). */
struct tcase {
	int        stmt;
	struct prm p[9];
	int        np;
	int        want_rows;
	int        want_chg;
	uint64_t   max_vis;
	int64_t    want_first;
	int        nosort;
};

static const struct tcase CASES[] = {
	{ 0,  { PI(1), PI(2), PI(3) }, 3, 1, -1, 2, INT64_MIN, 0 },
	{ 1,  { PI(2), PI(1) }, 2, 1, -1, 1, 31, 0 },
	{ 2,  { PI(31), PI(2), PI(1) }, 3, -1, 1, 2, INT64_MIN, 0 },
	{ 32, { PI(2), PI(1) }, 2, 1, -1, 1, 32, 0 },
	{ 3,  { PI(31), PI(2), PI(1), PI(4), PS("2026-01-02"), PI(5), PI(1) },
	      7, -1, 1, 2, INT64_MIN, 0 },
	{ 4,  { PI(31), PI(2), PI(1) }, 3, -1, 1, 2, INT64_MIN, 0 },
	{ 5,  { PI(77) }, 1, 1, -1, 1, INT64_MIN, 0 },
	{ 6,  { PI(77), PI(1) }, 2, 1, -1, 1, INT64_MIN, 0 },
	{ 7,  { PI(50), PI(77), PI(1) }, 3, -1, 1, 2, INT64_MIN, 0 },
	{ 8,  { PI(31), PI(2), PI(1), PI(1), PI(77), PI(1), PI(5), PR(12.5),
	        PS("dd") }, 9, -1, 1, 2, INT64_MIN, 0 },
	{ 9,  { PR(10.0), PI(1) }, 2, -1, 1, 2, INT64_MIN, 0 },
	{ 10, { PI(1) }, 1, 1, -1, 1, INT64_MIN, 0 },
	{ 11, { PR(5.0), PI(1), PI(2) }, 3, -1, 1, 2, INT64_MIN, 0 },
	{ 12, { PI(1), PI(2) }, 2, 1, -1, 1, INT64_MIN, 0 },
	{ 13, { PI(1), PI(2), PS("BARBARBAR") }, 3, 1, -1, 8, 7, 0 },
	{ 14, { PI(1), PI(2), PS("BARBARBAR") }, 3, 7, -1, 8, INT64_MIN, 1 },
	{ 15, { PI(1), PI(2), PI(3) }, 3, 1, -1, 1, INT64_MIN, 0 },
	{ 16, { PI(1), PI(2), PI(3) }, 3, 1, -1, 1, INT64_MIN, 0 },
	{ 17, { PR(1.5), PS("cdata"), PI(1), PI(2), PI(3) }, 5, -1, 1, 2,
	      INT64_MIN, 0 },
	{ 18, { PR(2.5), PI(1), PI(2), PI(3) }, 4, -1, 1, 2, INT64_MIN, 0 },
	{ 19, { PI(2), PI(1), PI(3), PI(2), PI(1), PS("2026"), PR(10.0),
	        PS("h") }, 8, -1, 1, 2, INT64_MIN, 0 },
	{ 20, { PI(1), PI(3), PS("ABLEPRIPRES") }, 3, 1, -1, 8, 8, 0 },
	{ 21, { PI(1), PI(3), PS("ABLEPRIPRES") }, 3, 8, -1, 8, INT64_MIN, 1 },
	{ 22, { PI(1), PI(2), PI(3) }, 3, 1, -1, 1, INT64_MIN, 0 },
	{ 23, { PI(1), PI(2), PI(4), PI(1), PI(2), PI(4) }, 6, 1, -1, 2, 31, 0 },
	{ 24, { PI(1), PI(2), PI(5) }, 3, 10, -1, 10, INT64_MIN, 0 },
	{ 25, { PI(2), PI(1) }, 2, 1, -1, 1, 22, 0 },
	{ 26, { PI(22), PI(2), PI(1) }, 3, -1, 1, 2, INT64_MIN, 0 },
	{ 25, { PI(2), PI(1) }, 2, 1, -1, 1, 23, 0 },
	{ 27, { PI(5), PI(2), PI(1) }, 3, 1, -1, 1, 6, 0 },
	{ 28, { PI(3), PI(5), PI(2), PI(1) }, 4, -1, 1, 2, INT64_MIN, 0 },
	{ 29, { PS("2026-02-02"), PI(5), PI(2), PI(1) }, 4, -1, 10, 20,
	      INT64_MIN, 0 },
	{ 30, { PI(5), PI(2), PI(1) }, 3, 1, -1, 10, INT64_MIN, 0 },
	{ 31, { PR(1.0), PI(3), PI(2), PI(1) }, 4, -1, 1, 2, INT64_MIN, 0 },
	{ 33, { PI(1), PI(2), PI(31), PI(31) }, 4, -1, -1, 160, INT64_MIN, 0 },
	{ 34, { PI(1), PI(77), PI(100) }, 3, 1, -1, 1, 1, 0 },
};

/* Order-line count of order o (5 .. 10). */
static int
ol_cnt(int o)
{
	return 5 + o % 6;
}

static void
load(sx_db *db)
{
	sx_stmt *st = NULL;
	char buf[64];
	int w, d, c, o, i, l;
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO warehouse VALUES(?,'wn','a','b','c','s',"
	    "'z',0.1,300000.0)", -1, &st, NULL) == SX_OK);
	for (w = 1; w <= N_W; w++) {
		sx_bind_int64(st, 1, w);
		CK(sx_step(st) == SX_DONE); sx_reset(st);
	}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO district VALUES(?,?,'dn','a','b','c',"
	    "'s','z',0.05,30000.0,31)", -1, &st, NULL) == SX_OK);
	for (w = 1; w <= N_W; w++)
		for (d = 1; d <= N_D; d++) {
			sx_bind_int64(st, 1, d); sx_bind_int64(st, 2, w);
			CK(sx_step(st) == SX_DONE); sx_reset(st);
		}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO customer VALUES(?,?,?,?,'OE',?,'a','b',"
	    "'c','s','z','555','2026','GC',50000,0.1,-10.0,10.0,1,0,'cd')", -1,
	    &st, NULL) == SX_OK);
	for (w = 1; w <= N_W; w++)
		for (d = 1; d <= N_D; d++)
			for (c = 1; c <= N_C; c++) {
				/* c_first distinct within a district: 7c mod 31. */
				snprintf(buf, sizeof buf, "F%02d", (c * 7) % 31);
				sx_bind_int64(st, 1, c); sx_bind_int64(st, 2, d);
				sx_bind_int64(st, 3, w); sx_bind_text(st, 4, buf, -1);
				sx_bind_text(st, 5, LASTS[c % 4], -1);
				CK(sx_step(st) == SX_DONE); sx_reset(st);
			}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO item VALUES(?,?,'iname',1.5,'idata')", -1,
	    &st, NULL) == SX_OK);
	for (i = 1; i <= N_I; i++) {
		sx_bind_int64(st, 1, i); sx_bind_int64(st, 2, i * 7);
		CK(sx_step(st) == SX_DONE); sx_reset(st);
	}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO stock VALUES(?,?,?,'d1','d2','d3','d4',"
	    "'d5','d6','d7','d8','d9','d10',0,0,0,'sd')", -1, &st, NULL) == SX_OK);
	for (w = 1; w <= N_W; w++)
		for (i = 1; i <= N_I; i++) {
			sx_bind_int64(st, 1, i); sx_bind_int64(st, 2, w);
			sx_bind_int64(st, 3, (i * 13) % 90 + 10);
			CK(sx_step(st) == SX_DONE); sx_reset(st);
		}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO orders VALUES(?,?,?,?,'2026',?,?,1)", -1,
	    &st, NULL) == SX_OK);
	for (w = 1; w <= N_W; w++)
		for (d = 1; d <= N_D; d++)
			for (o = 1; o <= N_O; o++) {
				/* customer o%10+1: three orders each for 1..10 */
				sx_bind_int64(st, 1, o); sx_bind_int64(st, 2, d);
				sx_bind_int64(st, 3, w); sx_bind_int64(st, 4, o % 10 + 1);
				if (o > 21) sx_bind_null(st, 5);
				else sx_bind_int64(st, 5, o % 10 + 1);
				sx_bind_int64(st, 6, ol_cnt(o));
				CK(sx_step(st) == SX_DONE); sx_reset(st);
			}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO new_orders VALUES(?,?,?)", -1, &st,
	    NULL) == SX_OK);
	for (w = 1; w <= N_W; w++)
		for (d = 1; d <= N_D; d++)
			for (o = 22; o <= N_O; o++) {
				sx_bind_int64(st, 1, o); sx_bind_int64(st, 2, d);
				sx_bind_int64(st, 3, w);
				CK(sx_step(st) == SX_DONE); sx_reset(st);
			}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO order_line VALUES(?,?,?,?,?,?,NULL,5,?,"
	    "'info')", -1, &st, NULL) == SX_OK);
	for (w = 1; w <= N_W; w++)
		for (d = 1; d <= N_D; d++)
			for (o = 1; o <= N_O; o++)
				for (l = 1; l <= ol_cnt(o); l++) {
					sx_bind_int64(st, 1, o); sx_bind_int64(st, 2, d);
					sx_bind_int64(st, 3, w); sx_bind_int64(st, 4, l);
					sx_bind_int64(st, 5, (o * l) % N_I + 1);
					sx_bind_int64(st, 6, w);
					sx_bind_double(st, 7, 1.25 * l);
					CK(sx_step(st) == SX_DONE); sx_reset(st);
				}
	sx_finalize(st);
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	size_t k;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-tpccseek");
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 2048; bo.cool_pct = 25;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK ||
	    sx_open_bt(bt, &db) != SX_OK) return 1;
	for (k = 0; k < sizeof SCHEMA / sizeof SCHEMA[0]; k++)
		CK(sx_exec(db, SCHEMA[k], NULL) == SX_OK);
	load(db);

	/* Every Turso statement, autocommit, in a New-Order / Payment /
	 * Order-Status / Delivery / Stock-Level order so the writes are
	 * read back by the statements that follow them. */
	printf("  stmt  rows  visited  sorts  (bound)\n");
	for (k = 0; k < sizeof CASES / sizeof CASES[0]; k++) {
		const struct tcase *tc = &CASES[k];
		int64_t first = INT64_MIN;
		uint64_t vis = 0, sorts = 0;
		int n = run(db, T[tc->stmt], tc->p, tc->np, &first, &vis, &sorts);
		printf("  %4d  %4d  %7llu  %5llu  (%llu)\n", tc->stmt, n,
		    (unsigned long long)vis, (unsigned long long)sorts,
		    (unsigned long long)tc->max_vis);
		if (n < 0) { CK(n >= 0); continue; }
		if (tc->want_rows >= 0) CK(n == tc->want_rows);
		if (tc->want_chg >= 0) CK(g_chg == tc->want_chg);
		if (tc->want_first != INT64_MIN) CK(first == tc->want_first);
		if (tc->nosort) CK(sorts == 0);
		if (vis > tc->max_vis)
			fprintf(stderr, "FAIL stmt %d visited %llu > %llu\n", tc->stmt,
			    (unsigned long long)vis, (unsigned long long)tc->max_vis);
		CK(vis <= tc->max_vis);
	}

	/* A hot row: Payment updates warehouse w_ytd on every transaction,
	 * so its row piles up versions until GC.  A point read must stop at
	 * the visible version, not walk every older one to end the range. */
	{
		static const struct prm up[2] = { PR(1.0), PI(2) };
		static const struct prm rd[1] = { PI(2) };
		uint64_t vis = 0;
		int i, n;
		for (i = 0; i < 50; i++)
			CK(run(db, T[9], up, 2, NULL, NULL, NULL) == 0);
		n = run(db, T[10], rd, 1, NULL, &vis, NULL);
		printf("  hot row: stmt 10 after 50 updates: rows %d visited %llu\n",
		    n, (unsigned long long)vis);
		CK(n == 1);
		CK(vis <= 1);
		n = run(db, T[9], up, 2, NULL, &vis, NULL);
		printf("  hot row: stmt 9 (update) visited %llu\n",
		    (unsigned long long)vis);
		CK(vis <= 2);
	}

	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	{ char wal[300]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("test_tpcc_seek: OK\n");
	return 0;
}
