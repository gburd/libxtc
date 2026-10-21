/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_prefix_split.c
 *	Leaves whose keys share a long prefix must split, fill, and lose
 *	nothing.
 *
 *	The leaf split costed every entry with its FULL key, not the
 *	suffix the node actually stores.  A secondary-index leaf (keys
 *	[0xFE][id][elem...][rowid], 30 shared bytes) holds far more
 *	entries than page / (slot + full key); counted with full keys its
 *	contents "exceed" two pages, so no cut was acceptable, the split
 *	failed (bt_insert -> XTC_E_INTERNAL), and xs_idx_maintain_ts --
 *	which ignores the result -- dropped the index entry: a query
 *	through the index then returned too few rows.  The same over-count
 *	left ascending index leaves 28-33% full (TPC-C's PK autoindexes).
 *
 *	(1) btree: 60,000 index-shaped keys, ascending and in a scrambled
 *	    order: every insert succeeds, every key is found, and the
 *	    ascending tree fills its leaves (>= 80%).
 *	(2) SQL: an index on (w, d, last) over 60,000 rows returns the same
 *	    count with and without the index.
 *	Off a loop.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "btnode.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define N 60000

/* [0xFE][idx 4][NUM w 11][NUM d 11][NUM id 11][rowid 8] = 46 bytes,
 * the shape of a three-column integer PK autoindex entry. */
static int
key_of(uint8_t *k, int w, int d, int id)
{
	const int v[3] = { w, d, id };
	int i, j, off = 5;
	uint64_t u;
	double dv;

	k[0] = 0xFE; k[1] = 0; k[2] = 0; k[3] = 0; k[4] = 5;
	for (i = 0; i < 3; i++) {
		dv = (double)v[i];
		memcpy(&u, &dv, 8);
		u |= 0x8000000000000000ull;
		k[off++] = 0x15;
		for (j = 7; j >= 0; j--) k[off + 7 - j] = (uint8_t)(u >> (8 * j));
		off += 8;
		k[off++] = 0x80; k[off++] = 0x00;
	}
	u = (uint64_t)(w * 100000 + d * 10000 + id) ^ 0x8000000000000000ull;
	for (j = 7; j >= 0; j--) k[off + 7 - j] = (uint8_t)(u >> (8 * j));
	return off + 8;
}

static int
run_btree(int scrambled, double min_fill)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bt_stats_t st;
	char path[256];
	uint8_t k[64];
	uint64_t used = 0;
	double fill;
	int fd, i, kl = 0, bad_ins = 0, bad_get = 0, rc;

	t_tmpl(path, sizeof path, "sqlxtc-pfxsplit");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 8192;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK) return 1;
	for (i = 0; i < N; i++) {
		/* scrambled: id order within each (w, d) run is a permutation */
		int j = scrambled ? (int)(((uint64_t)i * 7919u) % N) : i;
		kl = key_of(k, 1 + j / 30000, 1 + (j / 3000) % 10, 1 + j % 3000);
		rc = bt_insert(bt, k, (uint16_t)kl, "", 0);
		if (rc != XTC_OK && bad_ins++ == 0)
			fprintf(stderr, "  insert %d failed: rc %d\n", i, rc);
		used += btnode_entry_cost((uint16_t)kl, 0);
	}
	for (i = 0; i < N; i++) {
		uint8_t out[8];
		uint16_t ol = 0;
		kl = key_of(k, 1 + i / 30000, 1 + (i / 3000) % 10, 1 + i % 3000);
		if (bt_lookup(bt, k, (uint16_t)kl, out, sizeof out, &ol) != XTC_OK)
			bad_get++;
	}
	bt_get_stats(bt, &st);
	/* Fill counts the FULL key bytes (what the entries would need with
	 * no prefix compression) against the pages allocated. */
	fill = (double)used / ((double)(st.splits + 1) * 4096.0);
	printf("  %-10s inserts failed %d, keys missing %d, splits %llu, "
	    "height %llu, fill (full keys) %.0f%%\n",
	    scrambled ? "scrambled" : "ascending", bad_ins, bad_get,
	    (unsigned long long)st.splits, (unsigned long long)st.height,
	    fill * 100.0);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	if (bad_ins || bad_get || fill < min_fill) {
		fprintf(stderr, "FAIL btree %s: %d inserts failed, %d keys missing, "
		    "fill %.0f%% (min %.0f%%)\n", scrambled ? "scrambled" : "ascending",
		    bad_ins, bad_get, fill * 100.0, min_fill * 100.0);
		return 1;
	}
	return 0;
}

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
run_sql(void)
{
	char path[256], wal[300], sql[256];
	sx_db *h = NULL;
	int64_t a, b;
	int fd, w, d, c, fail = 0;

	t_tmpl(path, sizeof path, "sqlxtc-pfxsql");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	if (sx_init() != SX_OK || sx_storage_open(path, 16384) != SX_OK ||
	    sx_open(":memory:", &h) != SX_OK)
		return 1;
	if (sx_exec(h, "CREATE TABLE c(id INTEGER NOT NULL, d INTEGER NOT NULL, "
	    "w INTEGER NOT NULL, last TEXT, PRIMARY KEY(w, d, id))", NULL) != SX_OK ||
	    sx_exec(h, "CREATE INDEX ci ON c(w, d, last)", NULL) != SX_OK)
		return 1;
	for (w = 1; w <= 2; w++)
		for (d = 1; d <= 10; d++) {
			(void)sx_exec(h, "BEGIN", NULL);
			for (c = 1; c <= 3000; c++) {
				snprintf(sql, sizeof sql, "INSERT INTO c VALUES(%d,%d,%d,'L%d')",
				    c, d, w, (c * 7919) % 1000);
				if (sx_exec(h, sql, NULL) != SX_OK) fail = 1;
			}
			(void)sx_exec(h, "COMMIT", NULL);
		}
	/* through the index (w, d equality + a range on last) vs the PK */
	a = one(h, "SELECT count(*) FROM c WHERE w = 1 AND d = 1 AND last >= 'L'");
	b = one(h, "SELECT count(*) FROM c WHERE w = 1 AND d = 1");
	printf("  sql: index range count %lld, pk count %lld (want 3000)\n",
	    (long long)a, (long long)b);
	if (fail || a != 3000 || b != 3000) {
		fprintf(stderr, "FAIL sql: index count %lld, pk count %lld, insert "
		    "failure %d\n", (long long)a, (long long)b, fail);
		fail = 1;
	}
	sx_close(h);
	(void)sx_storage_close();
	unlink(path);
	unlink(wal);
	return fail;
}

int
main(void)
{
	int fail = 0;

	printf("test_btree_prefix_split\n");
	fail |= run_btree(0, 0.80);
	fail |= run_btree(1, 0.50);
	fail |= run_sql();
	if (fail) {
		fprintf(stderr, "FAIL: test_btree_prefix_split\n");
		return 1;
	}
	printf("  ok   prefix-compressed leaves split, fill, and keep every entry\n");
	return 0;
}
