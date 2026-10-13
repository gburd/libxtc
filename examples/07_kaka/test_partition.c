/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_partition.c -- standalone tests for the in-memory partition
 *	log core (plog_*).  No proc, no socket; exercises append,
 *	offset assignment, read-by-offset, high-water, and growth past
 *	the initial capacity.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "partition.h"
#include "t_assert.h"


static kaka_record_t
mkrec(const char *k, const char *v)
{
	kaka_record_t r;
	r.key = (const uint8_t *)k;
	r.key_len = k ? (uint32_t)strlen(k) : 0;
	r.value = (const uint8_t *)v;
	r.value_len = v ? (uint32_t)strlen(v) : 0;
	return r;
}

static void
test_append_offsets(void)
{
	plog_t *l;
	kaka_record_t r;
	ASSERT(plog_create(&l) == 0);
	ASSERT(plog_high_water(l) == 0);
	r = mkrec("a", "1"); ASSERT(plog_append(l, &r) == 0);
	r = mkrec("b", "2"); ASSERT(plog_append(l, &r) == 1);
	r = mkrec("c", "3"); ASSERT(plog_append(l, &r) == 2);
	ASSERT(plog_high_water(l) == 3);
	ASSERT(plog_count(l) == 3);
	plog_destroy(l);
	printf("  ok   append_offsets\n");
}

static void
test_read_by_offset(void)
{
	plog_t *l;
	kaka_record_t r, got;
	ASSERT(plog_create(&l) == 0);
	r = mkrec("key", "value"); plog_append(l, &r);
	r = mkrec(NULL, "novalkey"); plog_append(l, &r);

	ASSERT(plog_read(l, 0, &got) == 1);
	ASSERT(got.key_len == 3 && memcmp(got.key, "key", 3) == 0);
	ASSERT(got.value_len == 5 && memcmp(got.value, "value", 5) == 0);

	ASSERT(plog_read(l, 1, &got) == 1);
	ASSERT(got.key_len == 0);
	ASSERT(got.value_len == 8 && memcmp(got.value, "novalkey", 8) == 0);

	/* past high-water -> 0 */
	ASSERT(plog_read(l, 2, &got) == 0);
	plog_destroy(l);
	printf("  ok   read_by_offset\n");
}

static void
test_growth(void)
{
	plog_t *l;
	int i;
	kaka_record_t r, got;
	char kbuf[32], vbuf[32];
	ASSERT(plog_create(&l) == 0);

	/* Append well past the initial 1024 capacity to force realloc. */
	for (i = 0; i < 5000; i++) {
		snprintf(kbuf, sizeof kbuf, "k%d", i);
		snprintf(vbuf, sizeof vbuf, "value-%d", i);
		r = mkrec(kbuf, vbuf);
		ASSERT(plog_append(l, &r) == i);
	}
	ASSERT(plog_high_water(l) == 5000);

	/* Spot-check several offsets survived the reallocs intact. */
	for (i = 0; i < 5000; i += 777) {
		snprintf(vbuf, sizeof vbuf, "value-%d", i);
		ASSERT(plog_read(l, (uint64_t)i, &got) == 1);
		ASSERT(got.value_len == strlen(vbuf));
		ASSERT(memcmp(got.value, vbuf, got.value_len) == 0);
	}
	plog_destroy(l);
	printf("  ok   growth_past_capacity\n");
}

static void
test_segmented_recovery(void)
{
	char dir[256];
	plog_t *l;
	kaka_record_t r, got;
	char kbuf[16], vbuf[32];
	int i;

	snprintf(dir, sizeof dir, "/tmp/kaka-plog-test-%d", (int)getpid());
	(void)mkdir(dir, 0700);

	/* Phase 1: persistent log, append 3000 records (forces a few
	 * segment rolls with a small threshold), close. */
	ASSERT(plog_create_ex(dir, 64 * 1024 /* 64 KiB roll */, &l) == 0);
	for (i = 0; i < 3000; i++) {
		snprintf(kbuf, sizeof kbuf, "k%d", i);
		snprintf(vbuf, sizeof vbuf, "value-%d", i);
		r = mkrec(kbuf, vbuf);
		ASSERT(plog_append(l, &r) == i);
	}
	ASSERT(plog_high_water(l) == 3000);
	plog_destroy(l);

	/* Phase 2: reopen; recovery must replay every record with the
	 * same offsets and bytes. */
	ASSERT(plog_create_ex(dir, 64 * 1024, &l) == 0);
	ASSERT(plog_high_water(l) == 3000);
	ASSERT(plog_count(l) == 3000);
	for (i = 0; i < 3000; i += 137) {
		snprintf(vbuf, sizeof vbuf, "value-%d", i);
		ASSERT(plog_read(l, (uint64_t)i, &got) == 1);
		ASSERT(got.value_len == strlen(vbuf));
		ASSERT(memcmp(got.value, vbuf, got.value_len) == 0);
	}

	/* Append continues past the recovered high-water mark. */
	r = mkrec("after", "restart");
	ASSERT(plog_append(l, &r) == 3000);
	ASSERT(plog_high_water(l) == 3001);
	plog_destroy(l);

	/* Phase 3: reopen once more; the post-restart append persisted. */
	ASSERT(plog_create_ex(dir, 64 * 1024, &l) == 0);
	ASSERT(plog_high_water(l) == 3001);
	ASSERT(plog_read(l, 3000, &got) == 1);
	ASSERT(got.value_len == 7 && memcmp(got.value, "restart", 7) == 0);
	plog_destroy(l);

	/* Clean up the segment files + dir. */
	{
		char cmd[320];
		snprintf(cmd, sizeof cmd, "rm -f %s/*.log", dir);
		if (system(cmd) != 0) { /* best effort cleanup */ }
		(void)rmdir(dir);
	}
	printf("  ok   segmented_recovery (3000 records, rolls, restart)\n");
}

/* Bounded memory under a produce flood: with persistence on, appending
 * far more than a couple segments' worth of records must keep the
 * IN-RAM working set bounded (records in closed segments are evicted),
 * while high-water keeps climbing and reads of evicted offsets still
 * succeed by falling back to the segment files.  This is the property
 * the README advertises ("broker RSS stays bounded under a producer
 * flood"). */
static void
test_bounded_memory(void)
{
	plog_t *l;
	kaka_record_t r, got;
	char dir[256], vbuf[64];
	int i;
	const int N = 20000;               /* >> 2 segments at a 32 KiB roll */
	uint64_t max_resident = 0;

	snprintf(dir, sizeof dir, "/tmp/kaka-bound-test-%d", (int)getpid());
	(void)mkdir(dir, 0700);
	ASSERT(plog_create_ex(dir, 32 * 1024 /* small roll => many segments */, &l) == 0);

	for (i = 0; i < N; i++) {
		snprintf(vbuf, sizeof vbuf, "value-%d-padded-to-force-rolls", i);
		r = mkrec("k", vbuf);
		ASSERT(plog_append(l, &r) == i);
		if (plog_mem_records(l) > max_resident)
			max_resident = plog_mem_records(l);
	}

	/* high-water reflects ALL appends... */
	ASSERT(plog_high_water(l) == (uint64_t)N);
	/* ...but the in-RAM window stayed BOUNDED far below N.  Each record
	 * is ~50 B, roll is 32 KiB => ~650 records/segment; ~2 segments in
	 * RAM => a few thousand, never the full 20000.  Assert < N/4 as a
	 * generous bound that still fails hard if eviction regresses. */
	ASSERT(max_resident < (uint64_t)N / 4);

	/* Reads of long-evicted early offsets still succeed (served from the
	 * segment files), returning the right bytes. */
	for (i = 0; i < N; i += 1637) {
		snprintf(vbuf, sizeof vbuf, "value-%d-padded-to-force-rolls", i);
		ASSERT(plog_read(l, (uint64_t)i, &got) == 1);
		ASSERT(got.value_len == strlen(vbuf));
		ASSERT(memcmp(got.value, vbuf, got.value_len) == 0);
	}
	plog_destroy(l);

	{
		char cmd[320];
		snprintf(cmd, sizeof cmd, "rm -f %s/*.log", dir);
		if (system(cmd) != 0) { /* best effort */ }
		(void)rmdir(dir);
	}
	printf("  ok   bounded_memory (%d appended, RAM peak %llu records, evicted reads OK)\n",
	    N, (unsigned long long)max_resident);
}

static void
rmdir_all(const char *dir)
{
	char cmd[320];
	snprintf(cmd, sizeof cmd, "find '%s' -delete 2>/dev/null", dir);
	if (system(cmd) != 0) { /* best effort */ }
}

static int g_syncs;

static int
count_sync(int fd)
{
	g_syncs++;
	return fsync(fd) == 0 ? 0 : -1;
}

static int
fail_sync(int fd)
{
	(void)fd;
	return -5;
}

/* plog_append alone is not durable; plog_sync is; a failed sync
 * fail-stops the log; a torn tail is truncated on reopen. */
static void
test_sync_and_torn_tail(void)
{
	char dir[256], path[320];
	plog_t *l;
	kaka_record_t r, got;
	FILE *f;
	int i;

	snprintf(dir, sizeof dir, "/tmp/kaka-sync-test-%d", (int)getpid());
	(void)mkdir(dir, 0700);
	plog_set_sync_hook(count_sync);
	ASSERT(plog_create_ex(dir, 0, &l) == 0);
	r = mkrec("k", "v0");
	ASSERT(plog_append(l, &r) == 0);
	ASSERT(plog_synced(l) == 0 && plog_high_water(l) == 1);
	g_syncs = 0;
	ASSERT(plog_sync(l) == 0);
	ASSERT(g_syncs == 1 && plog_synced(l) == 1);
	for (i = 1; i < 10; i++) {
		r = mkrec("k", "vN");
		ASSERT(plog_append(l, &r) == i);
	}
	ASSERT(plog_sync(l) == 0 && g_syncs == 2);   /* one sync, 9 records */
	plog_set_sync_hook(fail_sync);
	r = mkrec("k", "lost");
	ASSERT(plog_append(l, &r) == 10);
	ASSERT(plog_sync(l) != 0 && plog_failed(l));
	ASSERT(plog_append(l, &r) < 0);              /* fail-stop */
	ASSERT(plog_synced(l) == 10);
	plog_set_sync_hook(count_sync);
	plog_destroy(l);

	/* Tear the last record: reopen keeps 0..9 (and drops 10 only if
	 * torn; here we append garbage after it). */
	snprintf(path, sizeof path, "%s/%020d.log", dir, 0);
	f = fopen(path, "ab");
	ASSERT(f != NULL);
	fwrite("\x01\x02\x03\x04garbage-tail", 1, 16, f);
	fclose(f);
	ASSERT(plog_create_ex(dir, 0, &l) == 0);
	ASSERT(plog_high_water(l) == 11);
	ASSERT(plog_read(l, 10, &got) == 1 && got.value_len == 4);
	r = mkrec("k", "after");
	ASSERT(plog_append(l, &r) == 11);
	plog_destroy(l);
	ASSERT(plog_create_ex(dir, 0, &l) == 0);
	ASSERT(plog_high_water(l) == 12);
	ASSERT(plog_read(l, 11, &got) == 1 && got.value_len == 5 &&
	    memcmp(got.value, "after", 5) == 0);
	plog_destroy(l);
	plog_set_sync_hook(NULL);
	rmdir_all(dir);
	printf("  ok   sync_and_torn_tail (ack needs sync, fail-stop, torn tail)\n");
}

/* Cold reads go through the sparse index: reading a deep offset of a
 * big closed segment costs at most ~IDX_INTERVAL bytes of scan, so many
 * random cold reads are fast; plog_fetch returns exact wire records. */
static void
test_index_and_fetch(void)
{
	char dir[256], v[600];
	plog_t *l;
	kaka_record_t r, got;
	uint8_t buf[8192];
	uint64_t off;
	uint32_t n;
	size_t need;
	long w;
	int i;

	snprintf(dir, sizeof dir, "/tmp/kaka-idx-test-%d", (int)getpid());
	(void)mkdir(dir, 0700);
	ASSERT(plog_create_ex(dir, 8u << 20, &l) == 0);   /* 8 MiB segment */
	memset(v, 'x', sizeof v);
	for (i = 0; i < 20000; i++) {
		snprintf(v, sizeof v, "%08d", i);
		v[8] = 'x';
		r.key = NULL; r.key_len = 0;
		r.value = (const uint8_t *)v; r.value_len = sizeof v;
		ASSERT(plog_append(l, &r) == i);
	}
	plog_evict_all(l);                  /* force every read cold */
	ASSERT(plog_mem_records(l) == 0 && plog_mem_bytes(l) == 0);
	for (i = 0; i < 20000; i += 7) {
		char want[9];
		snprintf(want, sizeof want, "%08d", i);
		ASSERT(plog_read(l, (uint64_t)i, &got) == 1);
		ASSERT(got.value_len == sizeof v && memcmp(got.value, want, 8) == 0);
	}
	off = 12345;
	w = plog_fetch(l, &off, 20000, buf, sizeof buf, &n, &need);
	ASSERT(n == 13 && off == 12358);              /* 13 x 616 B fit */
	ASSERT(w == 13 * (16 + 600) && kaka_get_u64(buf) == 12345);
	off = 19995;
	w = plog_fetch(l, &off, 20000, buf, sizeof buf, &n, &need);
	ASSERT(n == 5 && off == 20000);               /* stops at end */
	off = 0;
	w = plog_fetch(l, &off, 20000, buf, 100, &n, &need);
	ASSERT(w == 0 && n == 0 && need == 616 && off == 0);
	plog_destroy(l);
	rmdir_all(dir);
	printf("  ok   index_and_fetch (2858 random cold reads, wire fetch)\n");
}

/* Size-based retention deletes the oldest closed segments; reads below
 * the retained start return 0; a reopen sees the same range. */
static void
test_retention(void)
{
	char dir[256], v[100];
	plog_t *l;
	kaka_record_t r, got;
	uint64_t start;
	int i;

	snprintf(dir, sizeof dir, "/tmp/kaka-ret-test-%d", (int)getpid());
	(void)mkdir(dir, 0700);
	ASSERT(plog_create_ex(dir, 4096, &l) == 0);
	plog_set_retention(l, 16384);
	memset(v, 'r', sizeof v);
	r.key = NULL; r.key_len = 0;
	r.value = (const uint8_t *)v; r.value_len = sizeof v;
	for (i = 0; i < 5000; i++)
		ASSERT(plog_append(l, &r) == i);
	ASSERT(plog_segments(l) <= 6);
	start = plog_start(l);
	ASSERT(start > 0 && start < 5000);
	ASSERT(plog_read(l, start - 1, &got) == 0);
	ASSERT(plog_read(l, start, &got) == 1);
	plog_destroy(l);
	ASSERT(plog_create_ex(dir, 4096, &l) == 0);
	ASSERT(plog_start(l) == start && plog_high_water(l) == 5000);
	plog_destroy(l);
	rmdir_all(dir);
	printf("  ok   retention (5000 x 100 B, 16 KiB cap, start=%llu)\n",
	    (unsigned long long)start);
}

int
main(void)
{
	test_append_offsets();
	test_read_by_offset();
	test_growth();
	test_segmented_recovery();
	test_bounded_memory();
	test_sync_and_torn_tail();
	test_index_and_fetch();
	test_retention();
	printf("All kaka partition tests passed.\n");
	return 0;
}
