/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_errno.c -- verifies M1_CLAIMS.md Tm-errno:
 *	the canonical errno -> XTC_E_* mapper and the embedder hook.
 */

#include "munit.h"
#include "xtc_int.h"
#include "os_errno.h"

#include <errno.h>

/* [Tm-errno-1] The built-in table maps the codes the OS layer relies on. */
static MunitResult
test_builtin_table(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_errno_map(EINVAL),  ==, XTC_E_INVAL);
	munit_assert_int(__os_errno_map(ENOMEM),  ==, XTC_E_NOMEM);
	munit_assert_int(__os_errno_map(ENOENT),  ==, XTC_E_NOTFOUND);
	munit_assert_int(__os_errno_map(EEXIST),  ==, XTC_E_INVAL);
	munit_assert_int(__os_errno_map(EAGAIN),  ==, XTC_E_AGAIN);
	munit_assert_int(__os_errno_map(ECANCELED), ==, XTC_E_ABORTED);
	munit_assert_int(__os_errno_map(ENOSYS),  ==, XTC_E_NOSYS);
	munit_assert_int(__os_errno_map(ERANGE),  ==, XTC_E_RANGE);

	/* Unmapped / misuse fold to XTC_E_IO -- never a success code. */
	munit_assert_int(__os_errno_map(EIO),  ==, XTC_E_IO);
	munit_assert_int(__os_errno_map(0),    ==, XTC_E_IO);
	munit_assert_int(__os_errno_map(999999), ==, XTC_E_IO);
	return MUNIT_OK;
}

/* A test hook: remap ENOENT to a distinct code, and defer everything
 * else (return 0) so the built-in table still applies. */
static int
hook_remap_enoent(int e)
{
	return e == ENOENT ? XTC_E_ABORTED : 0;
}

/* A blanket hook: claim every code (never defers). */
static int
hook_all(int e)
{
	(void)e;
	return XTC_E_RESOURCE;
}

/* [Tm-errno-2] The hook overrides; returning 0 defers to the table;
 * NULL restores the default. */
static MunitResult
test_hook(const MunitParameter p[], void *d)
{
	(void)p; (void)d;

	/* No hook by default. */
	munit_assert_true(__os_errno_get_hook() == NULL);

	__os_errno_set_hook(hook_remap_enoent);
	munit_assert_true(__os_errno_get_hook() == hook_remap_enoent);
	/* Overridden code takes the hook's value ... */
	munit_assert_int(__os_errno_map(ENOENT), ==, XTC_E_ABORTED);
	/* ... but a code the hook defers on (returns 0) uses the table. */
	munit_assert_int(__os_errno_map(ENOMEM), ==, XTC_E_NOMEM);
	munit_assert_int(__os_errno_map(EINVAL), ==, XTC_E_INVAL);

	/* A blanket hook wins for everything. */
	__os_errno_set_hook(hook_all);
	munit_assert_int(__os_errno_map(ENOMEM), ==, XTC_E_RESOURCE);
	munit_assert_int(__os_errno_map(EINVAL), ==, XTC_E_RESOURCE);

	/* NULL restores the built-in table. */
	__os_errno_set_hook(NULL);
	munit_assert_true(__os_errno_get_hook() == NULL);
	munit_assert_int(__os_errno_map(ENOENT), ==, XTC_E_NOTFOUND);
	munit_assert_int(__os_errno_map(ENOMEM), ==, XTC_E_NOMEM);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Tm_errno_1_table", test_builtin_table, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm_errno_2_hook",  test_hook,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/errno", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
