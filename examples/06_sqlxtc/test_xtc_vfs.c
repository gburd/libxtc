/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test_xtc_vfs.c
 *	In-process test of the xtc-native SQLite VFS.  Registers the
 *	"xtc" VFS, opens a file-backed database through it, runs a
 *	create / insert / select workload via SQLite's own C API (no
 *	network, no daemon), and asserts the VFS actually carried the
 *	I/O -- read and write counters and byte volumes are non-zero,
 *	and the data round-trips.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlite/sqlite3.h"
#include "xtc_vfs.h"

static int g_sum;

static int
sum_cb(void *unused, int ncol, char **vals, char **names)
{
	(void)unused; (void)names;
	if (ncol >= 1 && vals[0] != NULL)
		g_sum = atoi(vals[0]);
	return 0;
}

static char g_mode[16];

static int
mode_cb(void *unused, int ncol, char **vals, char **names)
{
	(void)unused; (void)names;
	if (ncol >= 1 && vals[0] != NULL) {
		strncpy(g_mode, vals[0], sizeof g_mode - 1);
		g_mode[sizeof g_mode - 1] = '\0';
	}
	return 0;
}

/* Exercise WAL mode through the xtc VFS: this drives the shared-memory
 * methods (xShmMap / xShmLock / xShmBarrier / xShmUnmap) that the
 * rollback-journal workload above never touches.  Returns 0 on pass. */
static int
wal_test(void)
{
	sqlite3 *db = NULL;
	char *err = NULL;
	char path[] = "/tmp/sqlxtc-vfs-wal-XXXXXX";
	int fd, rc, i, fails = 0;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	unlink(path);

	rc = sqlite3_open_v2(path, &db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "xtc");
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL(wal): open: %s\n", sqlite3_errmsg(db));
		unlink(path);
		return 1;
	}

	g_mode[0] = '\0';
	rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL;", mode_cb, 0, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL(wal): set WAL: %s\n", err);
		sqlite3_free(err); sqlite3_close(db); unlink(path); return 1;
	}

	(void)sqlite3_exec(db,
	    "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);", 0, 0, 0);
	(void)sqlite3_exec(db, "BEGIN;", 0, 0, 0);
	for (i = 1; i <= 300; i++) {
		char sql[96];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(a,b) VALUES(%d,'wal-%d');", i, i);
		if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
			fprintf(stderr, "FAIL(wal): insert: %s\n", err);
			sqlite3_free(err); fails++; break;
		}
	}
	(void)sqlite3_exec(db, "COMMIT;", 0, 0, 0);

	g_sum = -1;
	(void)sqlite3_exec(db, "SELECT sum(a) FROM t;", sum_cb, 0, 0);
	sqlite3_close(db);

	if (strcmp(g_mode, "wal") != 0) {
		fprintf(stderr, "FAIL(wal): journal_mode=%s, expected wal\n",
		    g_mode);
		fails++;
	} else if (g_sum != 45150) {  /* sum(1..300) */
		fprintf(stderr, "FAIL(wal): sum=%d expected 45150\n", g_sum);
		fails++;
	} else {
		printf("  ok   WAL works through xtc vfs "
		       "(shm methods exercised, sum=%d)\n", g_sum);
	}

	unlink(path);
	{ char p2[300]; snprintf(p2, sizeof p2, "%s-wal", path); unlink(p2);
	  snprintf(p2, sizeof p2, "%s-shm", path); unlink(p2); }
	return fails;
}

int
main(void)
{
	sqlite3 *db = NULL;
	char *err = NULL;
	char path[] = "/tmp/sqlxtc-vfs-test-XXXXXX";
	int fd, rc, i, fails = 0;
	xtc_vfs_stats_t s0, s1;

	/* Unique temp path; remove the placeholder so SQLite creates it. */
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 2; }
	close(fd);
	unlink(path);

	if (xtc_vfs_register(0) != SQLITE_OK) {
		fprintf(stderr, "FAIL: xtc_vfs_register\n");
		return 2;
	}

	xtc_vfs_get_stats(&s0);

	rc = sqlite3_open_v2(path, &db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "xtc");
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL: open via xtc vfs: %s\n",
		    sqlite3_errmsg(db));
		return 2;
	}

	/* Force real file I/O: rollback journal, normal sync. */
	(void)sqlite3_exec(db, "PRAGMA journal_mode=DELETE;", 0, 0, 0);
	(void)sqlite3_exec(db, "PRAGMA synchronous=FULL;", 0, 0, 0);

	rc = sqlite3_exec(db,
	    "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);", 0, 0, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL: create: %s\n", err);
		sqlite3_free(err);
		return 2;
	}

	for (i = 1; i <= 500; i++) {
		char sql[96];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(a,b) VALUES(%d,'row-%d');", i, i);
		rc = sqlite3_exec(db, sql, 0, 0, &err);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "FAIL: insert %d: %s\n", i, err);
			sqlite3_free(err);
			return 2;
		}
	}

	g_sum = -1;
	rc = sqlite3_exec(db, "SELECT sum(a) FROM t;", sum_cb, 0, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL: select: %s\n", err);
		sqlite3_free(err);
		return 2;
	}

	sqlite3_close(db);
	xtc_vfs_get_stats(&s1);

	/* sum(1..500) == 125250: the data round-tripped through the VFS. */
	if (g_sum != 125250) {
		fprintf(stderr, "FAIL: sum(a)=%d expected 125250\n", g_sum);
		fails++;
	} else {
		printf("  ok   data round-trips through xtc vfs (sum=%d)\n",
		    g_sum);
	}

	if (s1.writes <= s0.writes || s1.bytes_written <= s0.bytes_written) {
		fprintf(stderr, "FAIL: no writes recorded (w=%llu b=%llu)\n",
		    (unsigned long long)s1.writes,
		    (unsigned long long)s1.bytes_written);
		fails++;
	} else {
		printf("  ok   vfs writes recorded (%llu calls, %llu bytes)\n",
		    (unsigned long long)s1.writes,
		    (unsigned long long)s1.bytes_written);
	}

	if (s1.reads <= s0.reads || s1.bytes_read <= s0.bytes_read) {
		fprintf(stderr, "FAIL: no reads recorded (r=%llu b=%llu)\n",
		    (unsigned long long)s1.reads,
		    (unsigned long long)s1.bytes_read);
		fails++;
	} else {
		printf("  ok   vfs reads recorded (%llu calls, %llu bytes)\n",
		    (unsigned long long)s1.reads,
		    (unsigned long long)s1.bytes_read);
	}

	printf("  info vfs syncs=%llu read_p50=%.1fus write_p50=%.1fus\n",
	    (unsigned long long)s1.syncs, s1.read_p50_us, s1.write_p50_us);

	unlink(path);

	/* WAL mode: exercises the VFS shared-memory forwarding. */
	fails += wal_test();

	printf("%s\n", fails == 0 ? "All xtc vfs tests passed."
	                          : "xtc vfs test FAILED.");
	return fails == 0 ? 0 : 1;
}
