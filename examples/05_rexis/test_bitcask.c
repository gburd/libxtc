/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_bitcask.c -- Bitcask KV unit tests.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "bitcask.h"
#include "t_assert.h"
#include "xtc.h"


static char *
mkdir_temp(void)
{
	static char dir[256];
	snprintf(dir, sizeof dir, "/tmp/bitcask-test-%d", (int)getpid());
	(void)rmdir(dir);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		perror("mkdir");
		exit(1);
	}
	return dir;
}

static void
rmrf(const char *dir)
{
	char path[512];
	snprintf(path, sizeof path, "%s/bitcask.data", dir);
	(void)unlink(path);
	(void)rmdir(dir);
}

static void
test_basic_put_get(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;

	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k1", 2, "value1", 6) == 0);
	ASSERT(bitcask_get(bc, "k1", 2, buf, sizeof buf, &n) == 0);
	ASSERT(n == 6);
	ASSERT(memcmp(buf, "value1", 6) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   basic_put_get\n");
}

static void
test_overwrite(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k", 1, "first", 5) == 0);
	ASSERT(bitcask_put(bc, "k", 1, "second", 6) == 0);
	ASSERT(bitcask_get(bc, "k", 1, buf, sizeof buf, &n) == 0);
	ASSERT(n == 6);
	ASSERT(memcmp(buf, "second", 6) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   overwrite\n");
}

static void
test_delete(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[16];
	size_t n;
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k", 1, "v", 1) == 0);
	ASSERT(bitcask_del(bc, "k", 1) == 0);
	ASSERT(bitcask_get(bc, "k", 1, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_del(bc, "k", 1) == 1);   /* second del = miss */
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   delete\n");
}

static void
test_recovery(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;
	int i;

	/* Phase 1: insert 100 keys, close. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 100; i++) {
		char k[16], v[32];
		snprintf(k, sizeof k, "key%d", i);
		snprintf(v, sizeof v, "value-%d", i);
		ASSERT(bitcask_put(bc, k, strlen(k), v, strlen(v)) == 0);
	}
	bitcask_close(bc);

	/* Phase 2: re-open, verify all 100 keys come back. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 100; i++) {
		char k[16], expect[32];
		snprintf(k, sizeof k, "key%d", i);
		snprintf(expect, sizeof expect, "value-%d", i);
		ASSERT(bitcask_get(bc, k, strlen(k), buf, sizeof buf, &n) == 0);
		ASSERT(n == strlen(expect));
		ASSERT(memcmp(buf, expect, n) == 0);
	}
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   recovery\n");
}

static void
test_recovery_with_deletes_and_overwrites(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;

	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_put(bc, "alpha",  5, "1", 1);
	bitcask_put(bc, "alpha",  5, "2", 1);    /* overwrite */
	bitcask_put(bc, "alpha",  5, "3", 1);    /* overwrite */
	bitcask_put(bc, "beta",   4, "B", 1);
	bitcask_put(bc, "gamma",  5, "G", 1);
	bitcask_del(bc, "beta",   4);             /* delete */
	bitcask_put(bc, "gamma",  5, "G2", 2);    /* overwrite */
	bitcask_close(bc);

	/* Recover. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_get(bc, "alpha", 5, buf, sizeof buf, &n) == 0);
	ASSERT(n == 1 && buf[0] == '3');
	ASSERT(bitcask_get(bc, "beta",  4, buf, sizeof buf, &n) == 1);   /* deleted */
	ASSERT(bitcask_get(bc, "gamma", 5, buf, sizeof buf, &n) == 0);
	ASSERT(n == 2 && memcmp(buf, "G2", 2) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   recovery_with_deletes_and_overwrites\n");
}

static int
__count_iter(const void *k, size_t kl, void *u)
{
	(void)k; (void)kl;
	(*(int *)u)++;
	return 0;
}

static void
test_iterate_and_stats(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_stats_t s;
	int count = 0;

	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_put(bc, "a", 1, "1", 1);
	bitcask_put(bc, "b", 1, "2", 1);
	bitcask_put(bc, "c", 1, "3", 1);
	bitcask_del(bc, "b", 1);

	bitcask_iterate(bc, __count_iter, &count);
	ASSERT(count == 2);

	bitcask_stat(bc, &s);
	ASSERT(s.n_keys == 2);
	ASSERT(s.puts == 3);
	ASSERT(s.dels == 1);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   iterate_and_stats\n");
}


static void
test_compaction(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_stats_t st;
	char key[32], buf[64];
	size_t n;
	int i;

	ASSERT(bitcask_open(dir, &bc) == 0);

	/* 200 keys, each overwritten 5x (lots of dead bytes), then a
	 * quarter deleted. */
	for (i = 0; i < 200; i++) {
		int r;
		snprintf(key, sizeof key, "key-%03d", i);
		for (r = 0; r < 5; r++) {
			char val[32];
			snprintf(val, sizeof val, "val-%03d-rev-%d", i, r);
			ASSERT(bitcask_put(bc, key, strlen(key),
			    val, strlen(val)) == 0);
		}
	}
	for (i = 0; i < 200; i += 4) {
		snprintf(key, sizeof key, "key-%03d", i);
		ASSERT(bitcask_del(bc, key, strlen(key)) == 0);
	}

	bitcask_stat(bc, &st);
	ASSERT(st.bytes_dead > 0);          /* garbage accumulated */

	ASSERT(bitcask_compact(bc) == 0);

	bitcask_stat(bc, &st);
	ASSERT(st.bytes_dead == 0);          /* reclaimed */

	/* Every surviving key reads back its last revision; deleted keys
	 * stay gone -- across compaction. */
	for (i = 0; i < 200; i++) {
		char want[32];
		snprintf(key, sizeof key, "key-%03d", i);
		snprintf(want, sizeof want, "val-%03d-rev-4", i);
		if (i % 4 == 0) {
			ASSERT(bitcask_get(bc, key, strlen(key), buf,
			    sizeof buf, &n) != 0);   /* deleted */
		} else {
			ASSERT(bitcask_get(bc, key, strlen(key), buf,
			    sizeof buf, &n) == 0);
			ASSERT(n == strlen(want));
			ASSERT(memcmp(buf, want, n) == 0);
		}
	}

	/* The compacted file survives a reopen (recovery from the merged
	 * data file). */
	bitcask_close(bc);
	ASSERT(bitcask_open(dir, &bc) == 0);
	snprintf(key, sizeof key, "key-077");
	ASSERT(bitcask_get(bc, key, strlen(key), buf, sizeof buf, &n) == 0);
	ASSERT(memcmp(buf, "val-077-rev-4", n) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   compaction\n");
}

/*
 * The in-memory index must grow with the key count.  A fixed 4,096-bucket
 * chained index walked ~N/4096 entries on EVERY put (to find the old
 * record), so a 5M-key load fell from 254k to 7.6k puts/s.  Time a fixed
 * batch of overwrites at 4k keys and at 400k keys; the index must keep
 * it within 8x (the working set is 100x larger, so some cache misses are
 * expected), and every key must survive the growth and a reopen.
 */
static void
test_index_grows(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_stats_t st;
	char key[32], buf[64];
	size_t n;
	int i, r, kl;
	int64_t t0, small_ns, big_ns;

	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 4000; i++) {
		kl = snprintf(key, sizeof key, "ik-%d", i);
		ASSERT(bitcask_put(bc, key, (size_t)kl, "v", 1) == 0);
	}
	t0 = xtc_clock_mono();
	for (r = 0; r < 10; r++)
		for (i = 0; i < 4000; i++) {
			kl = snprintf(key, sizeof key, "ik-%d", i);
			ASSERT(bitcask_size(bc, key, (size_t)kl, &n) == 0);
		}
	small_ns = xtc_clock_mono() - t0;
	for (i = 4000; i < 400000; i++) {
		kl = snprintf(key, sizeof key, "ik-%d", i);
		ASSERT(bitcask_put(bc, key, (size_t)kl, "v", 1) == 0);
	}
	t0 = xtc_clock_mono();
	for (r = 0; r < 10; r++)
		for (i = 0; i < 4000; i++) {
			kl = snprintf(key, sizeof key, "ik-%d",
			    (int)(((long)i * 97) % 400000));
			ASSERT(bitcask_size(bc, key, (size_t)kl, &n) == 0);
		}
	big_ns = xtc_clock_mono() - t0;
	printf("  40k index lookups: %.1f ms at 4k keys, %.1f ms at 400k "
	    "keys (%.1fx)\n", (double)small_ns / 1e6, (double)big_ns / 1e6,
	    (double)big_ns / (double)(small_ns > 0 ? small_ns : 1));
	ASSERT(big_ns < 8 * small_ns);
	bitcask_stat(bc, &st);
	ASSERT(st.n_keys == 400000);
	bitcask_close(bc);

	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_stat(bc, &st);
	ASSERT(st.n_keys == 400000);
	for (i = 0; i < 400000; i += 7) {
		kl = snprintf(key, sizeof key, "ik-%d", i);
		ASSERT(bitcask_get(bc, key, (size_t)kl, buf, sizeof buf,
		    &n) == 0);
		ASSERT(n == 1 && buf[0] == 'v');
	}
	ASSERT(bitcask_compact(bc) == 0);
	kl = snprintf(key, sizeof key, "ik-%d", 399999);
	ASSERT(bitcask_get(bc, key, (size_t)kl, buf, sizeof buf, &n) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   index_grows\n");
}

int
main(void)
{
	test_basic_put_get();
	test_overwrite();
	test_delete();
	test_recovery();
	test_recovery_with_deletes_and_overwrites();
	test_iterate_and_stats();
	test_compaction();
	test_index_grows();
	printf("All bitcask tests passed.\n");
	return 0;
}
