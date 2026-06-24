/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_xstore_scan.c
 *	The storage-native scan (xstore_scan_*) returns exactly the rows a
 *	VDBE SELECT returns, under MVCC snapshot visibility -- the read
 *	path the vectorized executor uses to bypass the VDBE.
 *
 *	Seed a table through the engine (so real MVCC versions, including
 *	an UPDATE that supersedes a version and a DELETE that tombstones
 *	one), then read it two ways: SELECT k, payload via the VDBE, and
 *	xstore_scan over the same B-tree.  Assert the scan yields the same
 *	(rowid, payload) set, including respecting the snapshot (newest
 *	non-tombstone version per rowid) and a rowid range bound.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

/* Collect (rowid -> a-value) from a VDBE SELECT into parallel arrays. */
struct kv { int64_t k; int64_t a; int has_a; };

static int
collect_vdbe(sx_db *db, const char *sql, struct kv *out, int cap)
{
	sx_stmt *st = NULL;
	int n = 0;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	while (sx_step(st) == SX_ROW && n < cap) {
		out[n].k = sx_column_int64(st, 0);
		if (sx_column_type(st, 1) == SX_NULL) { out[n].has_a = 0; out[n].a = 0; }
		else { out[n].has_a = 1; out[n].a = sx_column_int64(st, 1); }
		n++;
	}
	sx_finalize(st);
	return n;
}

static int
find_kv(const struct kv *arr, int n, int64_t k, struct kv *out)
{
	int i;
	for (i = 0; i < n; i++) if (arr[i].k == k) { *out = arr[i]; return 1; }
	return 0;
}

int
main(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[64] = "/tmp/sqlxtc_scanXXXXXX";
	int fd;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	struct kv ref[256];
	int nref, i;
	xstore_scan_t *sc;
	int64_t rid; const uint8_t *rec; int reclen, nscan = 0;

	fd = mkstemp(path); if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 64; bo.lsn_off = 0;
	CK(bm_create(&bo, &bm) == XTC_OK, "bm_create");
	CK(bt_open(bm, &bt) == XTC_OK, "bt_open");

	CK(sx_open_bt(bt, &db) == SX_OK, "open");
	/* The connection -> bt accessor returns the registered B-tree, so an
	 * executor with only the connection handle can open a scan. */
	CK(xstore_bt_of(db) == bt, "xstore_bt_of returns the registered bt");
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, a)", NULL)
	    == SX_OK, "create table");

	/* Seed: 20 rows; some a-values NULL. */
	CK(sx_prepare(db, "INSERT INTO t(k,a) VALUES(?,?)", -1, &st, NULL) == SX_OK, "prep ins");
	for (i = 1; i <= 20; i++) {
		sx_reset(st);
		sx_bind_int64(st, 1, i);
		if (i % 5 == 0) sx_bind_null(st, 2);
		else sx_bind_int64(st, 2, i * 100);
		CK(sx_step(st) == SX_DONE, "ins step");
	}
	sx_finalize(st); st = NULL;

	/* An UPDATE (supersede a version) and a DELETE (tombstone). */
	CK(sx_exec(db, "UPDATE t SET a = 7777 WHERE k = 3", NULL) == SX_OK, "update");
	CK(sx_exec(db, "DELETE FROM t WHERE k = 10", NULL) == SX_OK, "delete");

	/* Reference: what the VDBE sees at the latest snapshot. */
	nref = collect_vdbe(db, "SELECT k, a FROM t", ref, 256);
	CK(nref == 19, "19 visible rows (20 - 1 deleted)");   /* k=10 gone */

	/* Storage-native scan over the same B-tree at the latest snapshot. */
	sc = xstore_scan_open(bt, "t", 0 /* latest */, 0, 0, 0, 0);
	CK(sc != NULL, "scan_open");
	if (sc) {
		while (xstore_scan_next(sc, &rid, &rec, &reclen) == 1) {
			struct kv r = { 0, 0, 0 };
			int64_t a = 0; double d; const uint8_t *p; int nn;
			int cls;
			nscan++;
			CK(find_kv(ref, nref, rid, &r), "scan rowid is VDBE-visible");
			/* Payload column 0 is `a` (the non-key column). */
			cls = xstore_rec_col(rec, reclen, 0, &a, &d, &p, &nn);
			if (r.has_a) {
				CK(cls == XSTORE_C_INT && a == r.a, "scan a matches VDBE");
			} else {
				CK(cls == XSTORE_C_NULL, "scan a is NULL like VDBE");
			}
			CK(rid != 10, "deleted rowid never returned");
		}
		xstore_scan_close(sc);
	}
	CK(nscan == nref, "scan visited the same number of rows as the VDBE");

	/* Verify k=3's UPDATE is reflected (newest version wins). */
	{
		struct kv r3 = { 0, 0, 0 };
		CK(find_kv(ref, nref, 3, &r3) && r3.has_a && r3.a == 7777, "vdbe sees updated k=3");
		sc = xstore_scan_open(bt, "t", 0, 3, 1, 3, 1);   /* range [3,3] */
		CK(sc != NULL, "scan_open range");
		if (sc) {
			int got = 0; int64_t a = 0; double d; const uint8_t *p; int nn;
			while (xstore_scan_next(sc, &rid, &rec, &reclen) == 1) {
				got++;
				CK(rid == 3, "range scan only rowid 3");
				CK(xstore_rec_col(rec, reclen, 0, &a, &d, &p, &nn) == XSTORE_C_INT &&
				   a == 7777, "range scan sees the UPDATE (7777)");
			}
			CK(got == 1, "range [3,3] returns exactly one row");
			xstore_scan_close(sc);
		}
	}

	/* Unknown table -> NULL. */
	CK(xstore_scan_open(bt, "nope", 0, 0, 0, 0, 0) == NULL, "unknown table -> NULL");

	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);

	if (g_fail) { fprintf(stderr, "  xstore_scan: FAILURES\n"); return 1; }
	printf("  ok   storage-native scan matches the VDBE under MVCC "
	       "(%d rows, update + delete + range honored)\n", nscan);
	printf("All sqlxtc storage-native scan tests passed.\n");
	return 0;
}
