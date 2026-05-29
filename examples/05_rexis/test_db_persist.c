/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_db_persist.c -- end-to-end test for rexis db_t persistence
 * via the embedded Bitcask backend.  Spans the full path that real
 * SET / GET / DEL go through (db_set_ex, db_del, db_get) so that
 * any breakage in the integration is caught here -- not from a
 * fragile shell test that backgrounds servers and reads RESP
 * over a TCP socket.
 *
 * Build: hooked from examples/05_rexis/Makefile (depends on the
 * libxtc build for xtc_lrlock_*).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "db.h"

#define ASSERT(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s:%d: %s\n", \
		    __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

static char dir[256];

static void
setup_dir(void)
{
	snprintf(dir, sizeof dir, "/tmp/rexis-persist-test-%d", (int)getpid());
	(void)unlink(dir);            /* in case it's a stale file */
	if (mkdir(dir, 0700) != 0) {
		char path[512];
		snprintf(path, sizeof path, "%s/bitcask.data", dir);
		(void)unlink(path);       /* directory exists; clean it */
	}
}

static void
teardown_dir(void)
{
	char path[512];
	snprintf(path, sizeof path, "%s/bitcask.data", dir);
	(void)unlink(path);
	(void)rmdir(dir);
}

static void
test_set_and_recover(void)
{
	db_t *db = NULL;
	db_opts_t opts = DB_OPTS_DEFAULT;
	int i;
	const char *val;
	size_t vlen;

	opts.persist_dir = dir;

	/* Phase 1: open, write 50 keys, close. */
	ASSERT(db_create(&opts, &db) == 0 /* XTC_OK */);
	for (i = 0; i < 50; i++) {
		char k[16], v[32];
		snprintf(k, sizeof k, "k%d", i);
		snprintf(v, sizeof v, "value-%d", i);
		ASSERT(db_set(db, k, strlen(k), v, strlen(v)) == 0);
	}
	db_destroy(db);
	db = NULL;

	/* Phase 2: re-open (simulates server restart), keys come back. */
	ASSERT(db_create(&opts, &db) == 0);
	for (i = 0; i < 50; i++) {
		char k[16], expect[32];
		snprintf(k, sizeof k, "k%d", i);
		snprintf(expect, sizeof expect, "value-%d", i);
		ASSERT(db_get(db, k, strlen(k), &val, &vlen) == 0);
		ASSERT(vlen == strlen(expect));
		ASSERT(memcmp(val, expect, vlen) == 0);
	}
	db_destroy(db);
	printf("  ok   set_and_recover\n");
}

static void
test_overwrite_and_recover(void)
{
	db_t *db = NULL;
	db_opts_t opts = DB_OPTS_DEFAULT;
	const char *val;
	size_t vlen;

	opts.persist_dir = dir;

	ASSERT(db_create(&opts, &db) == 0);
	db_set(db, "k", 1, "first", 5);
	db_set(db, "k", 1, "second", 6);
	db_set(db, "k", 1, "third", 5);
	db_destroy(db);

	ASSERT(db_create(&opts, &db) == 0);
	ASSERT(db_get(db, "k", 1, &val, &vlen) == 0);
	ASSERT(vlen == 5);
	ASSERT(memcmp(val, "third", 5) == 0);
	db_destroy(db);
	printf("  ok   overwrite_and_recover\n");
}

static void
test_delete_persists(void)
{
	db_t *db = NULL;
	db_opts_t opts = DB_OPTS_DEFAULT;
	const char *val;
	size_t vlen;

	opts.persist_dir = dir;

	ASSERT(db_create(&opts, &db) == 0);
	db_set(db, "alpha", 5, "1", 1);
	db_set(db, "beta",  4, "2", 1);
	db_set(db, "gamma", 5, "3", 1);
	ASSERT(db_del(db, "beta", 4) == 1);
	db_destroy(db);

	ASSERT(db_create(&opts, &db) == 0);
	ASSERT(db_get(db, "alpha", 5, &val, &vlen) == 0);
	ASSERT(vlen == 1 && val[0] == '1');
	ASSERT(db_get(db, "beta",  4, &val, &vlen) == -1);  /* deleted */
	ASSERT(db_get(db, "gamma", 5, &val, &vlen) == 0);
	ASSERT(vlen == 1 && val[0] == '3');
	db_destroy(db);
	printf("  ok   delete_persists\n");
}

static void
test_persist_disabled(void)
{
	db_t *db = NULL;
	db_opts_t opts = DB_OPTS_DEFAULT;
	const char *val;
	size_t vlen;

	/* persist_dir = NULL: writes are memory-only.  Reopen brings
	 * back nothing, but no on-disk file is created either. */
	ASSERT(db_create(&opts, &db) == 0);
	db_set(db, "k", 1, "v", 1);
	ASSERT(db_get(db, "k", 1, &val, &vlen) == 0);  /* in-memory hit */
	db_destroy(db);
	printf("  ok   persist_disabled\n");
}

int
main(void)
{
	setup_dir();
	test_set_and_recover();
	teardown_dir();

	setup_dir();
	test_overwrite_and_recover();
	teardown_dir();

	setup_dir();
	test_delete_persists();
	teardown_dir();

	test_persist_disabled();

	printf("All db persistence tests passed.\n");
	return 0;
}
