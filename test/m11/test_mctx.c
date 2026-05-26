/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m11/test_mctx.c — verifies M11 memory contexts.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_mctx.h"
#include "xtc_int.h"

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m;
	void *a, *b;
	(void)p; (void)d;

	munit_assert_int(xtc_mctx_create(NULL, "root", 0, &m), ==, XTC_OK);
	munit_assert_string_equal(xtc_mctx_name(m), "root");

	a = xtc_mctx_alloc(m, 100);
	munit_assert_not_null(a);
	memset(a, 'A', 100);

	b = xtc_mctx_calloc(m, 50, sizeof(int));
	munit_assert_not_null(b);
	{
		int i;
		for (i = 0; i < 50; i++)
			munit_assert_int(((int *)b)[i], ==, 0);
	}

	munit_assert_size(xtc_mctx_total_chunks(m), ==, 2);
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 100 + 50 * sizeof(int));

	/* Free one early. */
	xtc_mctx_free(m, a);
	munit_assert_size(xtc_mctx_total_chunks(m), ==, 1);

	xtc_mctx_destroy(m);
	return MUNIT_OK;
}

/* Cleanup callback fires before chunks are freed. */
static int g_cleanup_count;
static void *g_cleanup_chunk;

static void
my_cleanup(void *u)
{
	(void)u;
	__os_atomic_fetch_add_i32(&g_cleanup_count, 1);
	/* The chunk is still alive when cleanup runs. */
	if (g_cleanup_chunk != NULL)
		((char *)g_cleanup_chunk)[0] = 'X';
}

static MunitResult
test_cleanup(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m;
	void *a;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_cleanup_count, 0);

	munit_assert_int(xtc_mctx_create(NULL, "scratch", 0, &m), ==, XTC_OK);
	a = xtc_mctx_alloc(m, 64);
	g_cleanup_chunk = a;
	munit_assert_int(xtc_mctx_register_cleanup(m, my_cleanup, NULL),
	    ==, XTC_OK);

	xtc_mctx_destroy(m);
	munit_assert_int(__os_atomic_load_i32(&g_cleanup_count), ==, 1);
	return MUNIT_OK;
}

/* Hierarchy: parent -> child -> grandchild.  Destroying parent
 * destroys grandchild's chunks too. */
static MunitResult
test_hierarchy(const MunitParameter p[], void *d)
{
	xtc_mctx_t *parent, *child, *grand;
	(void)p; (void)d;

	munit_assert_int(xtc_mctx_create(NULL,   "p", 0, &parent), ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(parent, "c", 0, &child),  ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(child,  "g", 0, &grand),  ==, XTC_OK);

	(void)xtc_mctx_alloc(parent, 100);
	(void)xtc_mctx_alloc(child,  200);
	(void)xtc_mctx_alloc(grand,  300);

	munit_assert_size(xtc_mctx_total_bytes(parent), ==, 100);
	munit_assert_size(xtc_mctx_total_bytes(child),  ==, 200);
	munit_assert_size(xtc_mctx_total_bytes(grand),  ==, 300);

	xtc_mctx_destroy(parent);
	/* No leaks; tested via not-segfaulting + total tally. */
	return MUNIT_OK;
}

/* Reset frees chunks but keeps the context usable.  Children are
 * cascaded-reset, not destroyed. */
static int g_reset_cleanup;
static void reset_cb(void *u) { (void)u; g_reset_cleanup++; }

static MunitResult
test_reset(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m, *child;
	(void)p; (void)d;

	g_reset_cleanup = 0;
	munit_assert_int(xtc_mctx_create(NULL, "m", 0, &m), ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(m,    "c", 0, &child), ==, XTC_OK);

	(void)xtc_mctx_alloc(m, 1000);
	(void)xtc_mctx_alloc(child, 500);
	(void)xtc_mctx_register_cleanup(m, reset_cb, NULL);

	xtc_mctx_reset(m);
	munit_assert_int(g_reset_cleanup, ==, 1);
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 0);
	munit_assert_size(xtc_mctx_total_bytes(child), ==, 0);

	/* Context is still usable. */
	munit_assert_not_null(xtc_mctx_alloc(m, 50));
	munit_assert_size(xtc_mctx_total_bytes(m), ==, 50);

	xtc_mctx_destroy(m);
	return MUNIT_OK;
}

static MunitResult
test_strdup(const MunitParameter p[], void *d)
{
	xtc_mctx_t *m;
	const char *s = "hello, world";
	char *copy;
	(void)p; (void)d;
	munit_assert_int(xtc_mctx_create(NULL, "m", 0, &m), ==, XTC_OK);
	copy = xtc_mctx_strdup(m, s);
	munit_assert_not_null(copy);
	munit_assert_string_equal(copy, s);
	munit_assert_ptr_not_equal(copy, s);
	xtc_mctx_destroy(m);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic",      test_basic,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cleanup",    test_cleanup,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hierarchy",  test_hierarchy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reset",      test_reset,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/strdup",     test_strdup,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m11/mctx", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
