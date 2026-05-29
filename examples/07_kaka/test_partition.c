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

#define ASSERT(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	exit(1); } } while (0)

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

int
main(void)
{
	test_append_offsets();
	test_read_by_offset();
	test_growth();
	test_segmented_recovery();
	printf("All kaka partition tests passed.\n");
	return 0;
}
