/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_bigtxn.c
 *	A transaction is one WAL record, so a big transaction (a bulk load:
 *	TPC-C's loader commits 100k Stock rows at once) is a big record.  It
 *	must commit, and it must survive a crash.
 *
 *	The log scan treated any record longer than WAL_MAX_REC (16 MiB) as a
 *	torn tail, so such a commit was acknowledged and then LOST by crash
 *	recovery, together with every commit after it (the reopen also
 *	truncated the log there).  A clean close hid it: the close compacts
 *	the log into small records.
 *
 *	Commits one ~24 MiB transaction (200k rows) and a small one after
 *	it, crashes (no flush, no clean flag, no compaction), recovers, and
 *	counts.  Then does the same while other threads commit small
 *	transactions concurrently with the big one.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define NBIG 200000

static int g_fail;
static char g_path[256];
static _Atomic int g_stop;
static _Atomic long g_small;

#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

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

static int
big_txn(sx_db *h, int base)
{
	sx_stmt *ins = NULL;
	int i, rc;
	if (sx_exec(h, "BEGIN", NULL) != SX_OK) return -1;
	if (sx_prepare(h, "INSERT INTO big VALUES(?, ?)", -1, &ins, NULL) != SX_OK) return -1;
	for (i = 0; i < NBIG; i++) {
		sx_bind_int64(ins, 1, base + i);
		sx_bind_text(ins, 2, "pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp", 80);
		if (sx_step(ins) != SX_DONE) { sx_finalize(ins); return -1; }
		sx_reset(ins);
	}
	sx_finalize(ins);
	rc = sx_exec(h, "COMMIT", NULL);
	return rc == SX_OK ? 0 : -1;
}

static void *
small_writer(void *arg)
{
	sx_db *h = NULL;
	char sql[96];
	long id = (long)arg, n = 0;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_stop)) {
		snprintf(sql, sizeof sql, "INSERT INTO small VALUES(%ld, %ld)", id * 1000000 + n, n);
		if (sx_exec(h, sql, NULL) == SX_OK) { atomic_fetch_add(&g_small, 1); n++; }
	}
	sx_close(h);
	return NULL;
}

int
main(void)
{
	char side[320];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *h = NULL;
	pthread_t th[3];
	long small_before;
	int fd, i;

	t_tmpl(g_path, sizeof g_path, "sqlxtc-bigtxn");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	unlink(g_path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(g_path, 4096) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE big(k INTEGER PRIMARY KEY, pad TEXT)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE small(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);

	/* (1) one big transaction, then a small one, then crash */
	CK(big_txn(h, 0) == 0);
	CK(sx_exec(h, "INSERT INTO small VALUES(-1, 1)", NULL) == SX_OK);
	sx_close(h);
	sx_storage_abandon();
	CK(sx_storage_open(g_path, 4096) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	printf("  after crash: big rows %lld (want %d), small row %lld (want 1)\n",
	    (long long)one(h, "SELECT count(*) FROM big"), NBIG,
	    (long long)one(h, "SELECT count(*) FROM small WHERE k = -1"));
	CK(one(h, "SELECT count(*) FROM big") == NBIG);
	CK(one(h, "SELECT count(*) FROM small WHERE k = -1") == 1);

	/* (2) a big transaction with small commits racing it, then crash */
	atomic_store(&g_stop, 0);
	for (i = 0; i < 3; i++) pthread_create(&th[i], NULL, small_writer, (void *)(long)(i + 1));
	CK(big_txn(h, NBIG) == 0);
	usleep(100 * 1000);
	atomic_store(&g_stop, 1);
	for (i = 0; i < 3; i++) pthread_join(th[i], NULL);
	small_before = atomic_load(&g_small);
	sx_close(h);
	sx_storage_abandon();
	CK(sx_storage_open(g_path, 4096) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	printf("  racing small commits: %ld acked, %lld after crash; big rows %lld (want %d)\n",
	    small_before, (long long)one(h, "SELECT count(*) FROM small WHERE k >= 0"),
	    (long long)one(h, "SELECT count(*) FROM big"), 2 * NBIG);
	CK(small_before > 0);
	CK(one(h, "SELECT count(*) FROM small WHERE k >= 0") == small_before);
	CK(one(h, "SELECT count(*) FROM big") == 2 * NBIG);
	sx_close(h);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) { snprintf(side, sizeof side, "%s%s", g_path, sfx[i]); unlink(side); }
	if (g_fail) { fprintf(stderr, "FAIL: test_wal_bigtxn\n"); return 1; }
	printf("  ok   a transaction bigger than the log buffer commits and survives a crash\n");
	return 0;
}
