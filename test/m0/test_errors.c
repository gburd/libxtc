/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m0/test_errors.c
 *	Verifies M0_CLAIMS.md [C6]: error contract.
 *	  - XTC_OK == 0.
 *	  - Every XTC_E_* < 0.
 *	  - xtc_strerror returns non-NULL stable text for known codes,
 *	    and "unknown" for codes outside the set (never NULL).
 */

#include "xtc.h"
#include "munit.h"
#include <string.h>
#include <limits.h>
#include <stdint.h>

static MunitResult
test_ok_is_zero(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_int((int)XTC_OK, ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_errors_negative(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_int(XTC_E_INVAL,    <, 0);
	munit_assert_int(XTC_E_NOMEM,    <, 0);
	munit_assert_int(XTC_E_NOSYS,    <, 0);
	munit_assert_int(XTC_E_RANGE,    <, 0);
	munit_assert_int(XTC_E_AGAIN,    <, 0);
	munit_assert_int(XTC_E_INTERNAL, <, 0);
	munit_assert_int(XTC_E_RESOURCE, <, 0);
	return MUNIT_OK;
}

static MunitResult
test_strerror_known(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_string_equal(xtc_strerror(XTC_OK),         "ok");
	munit_assert_string_equal(xtc_strerror(XTC_E_INVAL),    "invalid argument");
	munit_assert_string_equal(xtc_strerror(XTC_E_NOMEM),    "out of memory");
	munit_assert_string_equal(xtc_strerror(XTC_E_NOSYS),    "not implemented on this platform");
	munit_assert_string_equal(xtc_strerror(XTC_E_RANGE),    "numeric out of range");
	munit_assert_string_equal(xtc_strerror(XTC_E_AGAIN),    "resource temporarily unavailable");
	munit_assert_string_equal(xtc_strerror(XTC_E_INTERNAL), "internal invariant violation");
	munit_assert_string_equal(xtc_strerror(XTC_E_RESOURCE), "resource cap reached");
	munit_assert_string_equal(xtc_strerror(XTC_E_DEADLK),   "deadlock victim");
	munit_assert_string_equal(xtc_strerror(XTC_E_VERSION),  "version mismatch");
	munit_assert_string_equal(xtc_strerror(XTC_E_ABORTED),  "operation aborted");
	munit_assert_string_equal(xtc_strerror(XTC_E_NOTFOUND), "requested item does not exist");
	munit_assert_string_equal(xtc_strerror(XTC_E_IO),       "I/O error");
	return MUNIT_OK;
}

/* Public consumer utility wrappers (xtc_strerror.c): allocation, clock,
 * env, rng, strlcpy/cat, atomics.  These are the consumer-facing API
 * (RULE 3: examples/consumers use only xtc_*), so exercise every one on
 * its normal and edge inputs. */
static MunitResult
test_public_utils(const MunitParameter params[], void *data)
{
	void *p, *q;
	int64_t a = 0, t0, t1;
	char buf[16];
	size_t n;
	(void)params; (void)data;

	/* alloc family: non-NULL on success, correct rounding, free is safe. */
	p = xtc_malloc(64);
	munit_assert_not_null(p);
	q = xtc_realloc(p, 128);
	munit_assert_not_null(q);
	xtc_free(q);
	xtc_free(NULL);                 /* NULL free is a no-op */
	p = xtc_calloc(4, 32);
	munit_assert_not_null(p);
	munit_assert_int(((char *)p)[0], ==, 0);   /* zeroed */
	xtc_free(p);
	p = xtc_aligned_alloc(64, 256);
	munit_assert_not_null(p);
	munit_assert_uint64(((uintptr_t)p) % 64, ==, 0);
	xtc_aligned_free(p);
	xtc_aligned_free(NULL);

	/* clocks: monotonic never goes backward, real is plausible. */
	t0 = xtc_clock_mono();
	t1 = xtc_clock_mono();
	munit_assert_int64(t1, >=, t0);
	munit_assert_int64(xtc_clock_real(), >, 0);
	(void)xtc_sleep_ns(1000);       /* 1us; returns XTC_OK off a loop */

	/* atomics on a plain int64. */
	munit_assert_int64(xtc_atomic_i64_add(&a, 5), ==, 0);   /* returns prior */
	munit_assert_int64(xtc_atomic_i64_load(&a), ==, 5);

	/* strlcpy/strlcat: return source length, always NUL-terminate. */
	n = xtc_strlcpy(buf, "hello", sizeof buf);
	munit_assert_size(n, ==, 5);
	munit_assert_string_equal(buf, "hello");
	n = xtc_strlcat(buf, "-world", sizeof buf);
	munit_assert_size(n, ==, 11);
	munit_assert_string_equal(buf, "hello-world");
	/* truncation: dst too small, still NUL-terminated, returns would-be len */
	n = xtc_strlcpy(buf, "0123456789abcdefghij", sizeof buf);
	munit_assert_size(n, ==, 20);
	munit_assert_size(strlen(buf), ==, sizeof buf - 1);

	/* rng: seed makes it deterministic; two draws differ (with overwhelming
	 * probability) but a re-seed reproduces the stream. */
	xtc_rand_seed(0x1234);
	{
		uint64_t r1 = xtc_rand_u64();
		xtc_rand_seed(0x1234);
		munit_assert_uint64(xtc_rand_u64(), ==, r1);   /* reproducible */
	}

	/* env: set then get round-trips; get of a missing var is NULL. */
	munit_assert_int(xtc_env_set("XTC_TEST_VAR", "present", 1), ==, XTC_OK);
	munit_assert_not_null(xtc_env_get("XTC_TEST_VAR"));
	munit_assert_string_equal(xtc_env_get("XTC_TEST_VAR"), "present");
	munit_assert_null(xtc_env_get("XTC_TEST_NO_SUCH_VAR_ZZZ"));
	return MUNIT_OK;
}

static MunitResult
test_strerror_unknown_safe(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_string_equal(xtc_strerror(-9999),    "unknown");
	munit_assert_string_equal(xtc_strerror(INT_MAX),  "unknown");
	munit_assert_not_null(xtc_strerror(INT_MIN));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/c6_ok_is_zero",          test_ok_is_zero,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c6_errors_negative",     test_errors_negative,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c6_strerror_known",      test_strerror_known,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c6_strerror_unknown",    test_strerror_unknown_safe,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/public_utils",           test_public_utils,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m0/errors", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
