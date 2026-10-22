/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/test_db_table.c
 *	Keyspace table behaviour at scale: active expiry visits the whole
 *	table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "t_assert.h"

static int
mk(char *buf, size_t cap, const char *pfx, int i)
{
	return snprintf(buf, cap, "%s%d", pfx, i);
}

/*
 * Active expiry must make progress over the WHOLE table.  It used to
 * restart at bucket 0 on every pass and stop after max_scan entries, so
 * with live keys in the first buckets the expired keys behind them were
 * never reclaimed.
 */
static void
test_expiry_covers_table(void)
{
	db_t *db = NULL;
	db_opts_t o = DB_OPTS_DEFAULT;
	char k[32];
	int i, n, tick, removed = 0;
	int64_t later;

	ASSERT(db_create(&o, &db) == 0);
	db_write_begin(db);
	for (i = 0; i < 1000; i++) {
		n = mk(k, sizeof k, "live", i);
		ASSERT(db_set(db, k, (size_t)n, "v", 1) == 0);
	}
	for (i = 0; i < 20000; i++) {
		n = mk(k, sizeof k, "ttl", i);
		ASSERT(db_set_ex(db, k, (size_t)n, "v", 1,
		    xtc_clock_mono() + 1) == 0);
	}
	db_write_end(db);
	later = xtc_clock_mono() + 1000000000LL;
	/* 100 entries per pass, as expire.c: enough passes to sweep the
	 * table a few times over. */
	for (tick = 0; tick < 2000 && db_key_count(db) > 1000; tick++) {
		db_write_begin(db);
		removed += db_expire_stale(db, later, 100);
		db_write_end(db);
	}
	printf("  reclaimed %d of 20000 expired keys in %d passes\n",
	    removed, tick);
	ASSERT(removed == 20000);
	ASSERT(db_key_count(db) == 1000);
	db_destroy(db);
	printf("  ok   expiry_covers_table\n");
}

int
main(void)
{
	test_expiry_covers_table();
	printf("All db table tests passed.\n");
	return 0;
}
