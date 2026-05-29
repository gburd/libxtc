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

int
main(void)
{
	test_append_offsets();
	test_read_by_offset();
	test_growth();
	printf("All kaka partition tests passed.\n");
	return 0;
}
