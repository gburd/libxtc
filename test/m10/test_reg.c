/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_reg.c -- verifies M10.5 process registry.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"

#include <stdint.h>
#include <stdio.h>

/* Mirror of reg.c's FNV-1a over a NUL-terminated name and its bucket
 * count, so the collision test can find names that land in the same
 * bucket without reaching into reg.c internals. */
#define REG_NBUCKETS 256u
static uint32_t
reg_hash(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s != '\0'; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

static MunitResult
test_reg_basic(const MunitParameter p[], void *d)
{
	xtc_reg_t *r;
	xtc_pid_t pid_a = { 0, 1, 1 }, pid_b = { 0, 2, 1 };
	xtc_pid_t got;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 0);

	munit_assert_int(xtc_reg_register(r, "alpha", pid_a), ==, XTC_OK);
	munit_assert_int(xtc_reg_register(r, "beta",  pid_b), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 2);

	/* Duplicate name rejected. */
	munit_assert_int(xtc_reg_register(r, "alpha", pid_b), ==, XTC_E_INVAL);

	/* whereis */
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_a));
	munit_assert_int(xtc_reg_whereis(r, "beta", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_b));
	munit_assert_int(xtc_reg_whereis(r, "missing", &got), ==, XTC_E_INVAL);

	/* unregister */
	munit_assert_int(xtc_reg_unregister(r, "alpha"), ==, XTC_OK);
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_unregister(r, "alpha"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_count(r), ==, 1);

	/* Re-register under new pid OK. */
	munit_assert_int(xtc_reg_register(r, "alpha", pid_b), ==, XTC_OK);
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_b));

	xtc_reg_destroy(r);
	return MUNIT_OK;
}

/*
 * Collision test: register several names that all hash to the same
 * bucket (hash & (REG_NBUCKETS - 1)), and verify each is found and
 * removed independently.  Exercises chain walking and unlink-by-link
 * at the head, middle, and tail of a chain.
 */
static MunitResult
test_reg_collisions(const MunitParameter p[], void *d)
{
#define NCOLL 5
	xtc_reg_t *r;
	char names[NCOLL][32];
	xtc_pid_t pids[NCOLL];
	xtc_pid_t got;
	uint32_t target;
	int found = 0;
	long k;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	/* Find NCOLL distinct names whose hash lands in one bucket. */
	target = reg_hash("k0") & (REG_NBUCKETS - 1);
	for (k = 0; found < NCOLL && k < 5000000L; k++) {
		char cand[32];
		(void)snprintf(cand, sizeof cand, "k%ld", k);
		if ((reg_hash(cand) & (REG_NBUCKETS - 1)) == target) {
			(void)snprintf(names[found], sizeof names[found],
			    "%s", cand);
			pids[found].loop_id = 0;
			pids[found].local_id = (uint16_t)(found + 1);
			pids[found].gen = 1;
			found++;
		}
	}
	munit_assert_int(found, ==, NCOLL);

	/* All collide in the same bucket. */
	for (i = 1; i < NCOLL; i++)
		munit_assert_uint32(reg_hash(names[i]) & (REG_NBUCKETS - 1),
		    ==, target);

	/* Register all, then confirm each resolves to its own pid. */
	for (i = 0; i < NCOLL; i++)
		munit_assert_int(xtc_reg_register(r, names[i], pids[i]), ==,
		    XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, NCOLL);
	for (i = 0; i < NCOLL; i++) {
		munit_assert_int(xtc_reg_whereis(r, names[i], &got), ==,
		    XTC_OK);
		munit_assert_true(xtc_pid_eq(got, pids[i]));
	}

	/* Remove the first-inserted (chain tail) and a middle node; the
	 * rest must still resolve. */
	munit_assert_int(xtc_reg_unregister(r, names[0]), ==, XTC_OK);
	munit_assert_int(xtc_reg_unregister(r, names[2]), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, NCOLL - 2);
	munit_assert_int(xtc_reg_whereis(r, names[0], &got), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_whereis(r, names[2], &got), ==, XTC_E_INVAL);
	for (i = 0; i < NCOLL; i++) {
		if (i == 0 || i == 2) continue;
		munit_assert_int(xtc_reg_whereis(r, names[i], &got), ==,
		    XTC_OK);
		munit_assert_true(xtc_pid_eq(got, pids[i]));
	}

	/* Remove the remaining nodes; bucket empties cleanly. */
	munit_assert_int(xtc_reg_unregister(r, names[1]), ==, XTC_OK);
	munit_assert_int(xtc_reg_unregister(r, names[3]), ==, XTC_OK);
	munit_assert_int(xtc_reg_unregister(r, names[4]), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 0);

	xtc_reg_destroy(r);
	return MUNIT_OK;
#undef NCOLL
}

/*
 * Scale test: register N names, look every one up, then unregister
 * them all.  Demonstrates the table works (and stays correct) far
 * past the bucket count, with chains hashed across all buckets.
 */
static MunitResult
test_reg_scale(const MunitParameter p[], void *d)
{
#define NSCALE 5000
	xtc_reg_t *r;
	xtc_pid_t got;
	char name[32];
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	for (i = 0; i < NSCALE; i++) {
		xtc_pid_t pid = { 0, (uint16_t)(i + 1), (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_register(r, name, pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_count(r), ==, NSCALE);

	/* Every name resolves to the pid it was registered with. */
	for (i = 0; i < NSCALE; i++) {
		xtc_pid_t want = { 0, (uint16_t)(i + 1), (uint32_t)(i + 1) };
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_whereis(r, name, &got), ==, XTC_OK);
		munit_assert_true(xtc_pid_eq(got, want));
	}

	/* A name that was never registered misses. */
	munit_assert_int(xtc_reg_whereis(r, "svc-none", &got), ==, XTC_E_INVAL);

	/* Unregister all; count returns to zero. */
	for (i = 0; i < NSCALE; i++) {
		(void)snprintf(name, sizeof name, "svc-%d", i);
		munit_assert_int(xtc_reg_unregister(r, name), ==, XTC_OK);
	}
	munit_assert_int(xtc_reg_count(r), ==, 0);
	munit_assert_int(xtc_reg_whereis(r, "svc-0", &got), ==, XTC_E_INVAL);

	xtc_reg_destroy(r);
	return MUNIT_OK;
#undef NSCALE
}

static MunitTest tests[] = {
	{ "/reg_basic", test_reg_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_collisions", test_reg_collisions, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_scale", test_reg_scale, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/reg", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
