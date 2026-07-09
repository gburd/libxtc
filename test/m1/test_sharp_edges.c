/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_sharp_edges.c -- the POSIX/libc "sharp edges" trio:
 *	thread-safe env access, a per-thread seedable PRNG, and BSD
 *	bounded string copy/cat.  Exercises the public xtc_* surface.
 */

#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc_int.h"

/* --- environment round-trip --- */
static MunitResult
test_env_roundtrip(const MunitParameter p[], void *d)
{
	const char *v;
	(void)p; (void)d;

	munit_assert_int(xtc_env_set("XTC_TEST_ENV", "hello", 1), ==, XTC_OK);
	v = xtc_env_get("XTC_TEST_ENV");
	munit_assert_not_null(v);
	munit_assert_string_equal(v, "hello");

	/* overwrite == 0 leaves the existing value alone. */
	munit_assert_int(xtc_env_set("XTC_TEST_ENV", "world", 0), ==, XTC_OK);
	v = xtc_env_get("XTC_TEST_ENV");
	munit_assert_string_equal(v, "hello");

	/* overwrite == 1 replaces it. */
	munit_assert_int(xtc_env_set("XTC_TEST_ENV", "world", 1), ==, XTC_OK);
	v = xtc_env_get("XTC_TEST_ENV");
	munit_assert_string_equal(v, "world");

	/* unset variable -> NULL. */
	munit_assert_null(xtc_env_get("XTC_TEST_ENV_MISSING_XYZ"));

	/* invalid names. */
	munit_assert_int(xtc_env_set(NULL, "x", 1), ==, XTC_E_INVAL);
	munit_assert_int(xtc_env_set("", "x", 1), ==, XTC_E_INVAL);
	munit_assert_int(xtc_env_set("bad=name", "x", 1), ==, XTC_E_INVAL);
	munit_assert_null(xtc_env_get(NULL));
	return MUNIT_OK;
}

/* --- RNG determinism: same seed -> same sequence --- */
static MunitResult
test_rand_determinism(const MunitParameter p[], void *d)
{
	uint64_t a[8], b[8];
	int i;
	(void)p; (void)d;

	xtc_rand_seed(0x1234567890abcdefULL);
	for (i = 0; i < 8; i++)
		a[i] = xtc_rand_u64();

	xtc_rand_seed(0x1234567890abcdefULL);
	for (i = 0; i < 8; i++)
		b[i] = xtc_rand_u64();

	for (i = 0; i < 8; i++)
		munit_assert_uint64(a[i], ==, b[i]);

	/* A different seed must not reproduce the same sequence (the odds
	 * of an 8-word collision from splitmix64 are astronomically low). */
	xtc_rand_seed(0x1234567890abcde0ULL);
	{
		int same = 1;
		for (i = 0; i < 8; i++)
			if (xtc_rand_u64() != a[i])
				same = 0;
		munit_assert_int(same, ==, 0);
	}
	return MUNIT_OK;
}

/* --- RNG: two threads have independent streams --- */
struct rng_thr_arg {
	uint64_t seed;
	uint64_t out[8];
};

static void *
rng_thr(void *arg)
{
	struct rng_thr_arg *a = arg;
	int i;
	xtc_rand_seed(a->seed);
	for (i = 0; i < 8; i++)
		a->out[i] = xtc_rand_u64();
	return NULL;
}

static MunitResult
test_rand_per_thread(const MunitParameter p[], void *d)
{
	__os_thread_t t1, t2;
	struct rng_thr_arg a1 = { 0xaaaaaaaaaaaaaaaaULL, {0} };
	struct rng_thr_arg a2 = { 0xbbbbbbbbbbbbbbbbULL, {0} };
	int i;
	(void)p; (void)d;

	munit_assert_int(__os_thread_create(&t1, rng_thr, &a1), ==, XTC_OK);
	munit_assert_int(__os_thread_create(&t2, rng_thr, &a2), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&t1, NULL), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&t2, NULL), ==, XTC_OK);

	/* Different seeds -> different streams (independent per-thread
	 * state, no cross-thread interference). */
	for (i = 0; i < 8; i++)
		if (a1.out[i] != a2.out[i])
			break;
	munit_assert_int(i, <, 8);

	/* Each thread's stream matches a fresh single-threaded run with the
	 * same seed: proof the threads did not share/clobber state. */
	{
		uint64_t ref[8];
		xtc_rand_seed(a1.seed);
		for (i = 0; i < 8; i++)
			ref[i] = xtc_rand_u64();
		for (i = 0; i < 8; i++)
			munit_assert_uint64(ref[i], ==, a1.out[i]);
	}
	return MUNIT_OK;
}

/* --- strlcpy: NUL-termination, truncation, return length --- */
static MunitResult
test_strlcpy(const MunitParameter p[], void *d)
{
	char buf[8];
	size_t r;
	(void)p; (void)d;

	/* Fits: full copy, return strlen(src). */
	r = xtc_strlcpy(buf, "abc", sizeof buf);
	munit_assert_size(r, ==, 3);
	munit_assert_string_equal(buf, "abc");

	/* Truncation: return is the length it TRIED to create, and dst is
	 * always NUL-terminated. */
	r = xtc_strlcpy(buf, "0123456789", sizeof buf);
	munit_assert_size(r, ==, 10);
	munit_assert_size(strlen(buf), ==, 7);   /* 7 chars + NUL == 8 */
	munit_assert_string_equal(buf, "0123456");

	/* Exact fit (7 chars + NUL). */
	r = xtc_strlcpy(buf, "1234567", sizeof buf);
	munit_assert_size(r, ==, 7);
	munit_assert_string_equal(buf, "1234567");

	/* dstsize 0: no write, return strlen(src). */
	r = xtc_strlcpy(buf, "xyz", 0);
	munit_assert_size(r, ==, 3);
	return MUNIT_OK;
}

/* --- strlcat: NUL-termination, truncation, return length --- */
static MunitResult
test_strlcat(const MunitParameter p[], void *d)
{
	char buf[8];
	size_t r;
	(void)p; (void)d;

	strcpy(buf, "ab");
	r = xtc_strlcat(buf, "cd", sizeof buf);
	munit_assert_size(r, ==, 4);
	munit_assert_string_equal(buf, "abcd");

	/* Truncating cat: return = strlen(dst) + strlen(src). */
	strcpy(buf, "abcd");
	r = xtc_strlcat(buf, "efghij", sizeof buf);
	munit_assert_size(r, ==, 10);            /* 4 + 6 */
	munit_assert_size(strlen(buf), ==, 7);   /* filled to 7 + NUL */
	munit_assert_string_equal(buf, "abcdefg");

	/* dst already full (no NUL room): report dstsize-ish + strlen(src)
	 * without touching dst beyond what fits. */
	memcpy(buf, "1234567", 8);   /* "1234567\0" */
	r = xtc_strlcat(buf, "89", sizeof buf);
	munit_assert_size(r, ==, 9);             /* 7 + 2 */
	munit_assert_string_equal(buf, "1234567");
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/env_roundtrip",   test_env_roundtrip,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rand_determinism", test_rand_determinism, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rand_per_thread", test_rand_per_thread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/strlcpy",         test_strlcpy,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/strlcat",         test_strlcat,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/sharp_edges", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
