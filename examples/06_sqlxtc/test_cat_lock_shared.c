/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_cat_lock_shared.c
 *	Warm statements of SEVERAL connections that share one OS thread take
 *	no global catalog lock.
 *
 *	In the server a loop thread runs many connection fibers.  The
 *	connection -> (bt, ctx) lookup (xs_dbmap_get, several per statement)
 *	kept a ONE-entry per-thread cache, so connections on one thread
 *	evicted each other on every statement and every statement took the
 *	global catalog amutex; so did the planner's index list on every
 *	write.  At 32 connections on 8 loops most connection fibers were
 *	parked on that lock and TPROC-C over Quack fell from 125k (8
 *	connections) to 31k tpmC.
 *
 *	Here: NC connections on the main thread, a table with a secondary
 *	and a unique index; warm each connection's statements, then
 *	interleave them round-robin (UPDATE, INSERT in a transaction, point
 *	SELECT) and count catalog-lock acquisitions: must be 0.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#define NC 8

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static int
round(sx_db **h, sx_stmt **upd, sx_stmt **ins, sx_stmt **sel, int r)
{
	int i, rc = 0;
	for (i = 0; i < NC; i++) {
		sx_bind_int64(upd[i], 1, (int64_t)(i * 1000 + r));
		sx_bind_int64(upd[i], 2, (int64_t)(i + 1));
		rc |= sx_step(upd[i]) != SX_DONE;
		sx_reset(upd[i]);
		rc |= sx_exec(h[i], "BEGIN", NULL) != SX_OK;
		sx_bind_int64(ins[i], 1, (int64_t)(100000 + r * NC + i));
		sx_bind_int64(ins[i], 2, (int64_t)(100000 + r * NC + i));
		rc |= sx_step(ins[i]) != SX_DONE;
		sx_reset(ins[i]);
		rc |= sx_exec(h[i], "COMMIT", NULL) != SX_OK;
		sx_bind_int64(sel[i], 1, (int64_t)(i + 1));
		rc |= sx_step(sel[i]) != SX_ROW;
		sx_reset(sel[i]);
	}
	return rc;
}

int
main(void)
{
	char path[256], side[320], q[128];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *h[NC];
	sx_stmt *upd[NC], *ins[NC], *sel[NC];
	uint64_t l0, locks;
	int fd, i, r;

	t_tmpl(path, sizeof path, "sqlxtc-catshared");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	if (sx_init() != SX_OK || sx_storage_open(path, 1024) != SX_OK)
		return 1;
	for (i = 0; i < NC; i++)
		if (sx_open(path, &h[i]) != SX_OK) return 1;
	CK(sx_exec(h[0], "CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(h[0], "CREATE INDEX t_v ON t(v)", NULL) == SX_OK);
	CK(sx_exec(h[0], "CREATE UNIQUE INDEX t_k ON t(k)", NULL) == SX_OK);
	for (i = 1; i <= NC; i++) {
		snprintf(q, sizeof q, "INSERT INTO t VALUES(%d, %d, 0)", i, i);
		CK(sx_exec(h[0], q, NULL) == SX_OK);
	}
	for (i = 0; i < NC; i++) {
		CK(sx_prepare(h[i], "UPDATE t SET v = ? WHERE id = ?", -1, &upd[i], NULL) == SX_OK);
		CK(sx_prepare(h[i], "INSERT INTO t(id, k, v) VALUES(?, ?, 1)", -1, &ins[i], NULL) == SX_OK);
		CK(sx_prepare(h[i], "SELECT v FROM t WHERE id = ?", -1, &sel[i], NULL) == SX_OK);
	}
	/* Warm: each connection's statements planned once. */
	CK(round(h, upd, ins, sel, 0) == 0);
	l0 = xstore_cat_lock_count();
	for (r = 1; r <= 20; r++)
		CK(round(h, upd, ins, sel, r) == 0);
	locks = xstore_cat_lock_count() - l0;
	printf("  %d connections on one thread, 20 interleaved rounds: "
	    "catalog-lock acquisitions %llu\n", NC, (unsigned long long)locks);
	CK(locks == 0);
	for (i = 0; i < NC; i++) {
		sx_finalize(upd[i]); sx_finalize(ins[i]); sx_finalize(sel[i]);
		sx_close(h[i]);
	}
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_cat_lock_shared\n");
		return 1;
	}
	printf("  ok   warm statements of connections sharing a thread take no "
	    "catalog lock\n");
	return 0;
}
