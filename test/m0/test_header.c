/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m0/test_header.c
 *	Verifies M0_CLAIMS.md [C4]: <xtc.h> alone is sufficient.
 *
 * The proof is mechanical: this translation unit includes ONLY
 * <xtc.h> (and standard headers it pulls itself), references
 * every M0-public symbol, and compiles + links.
 */

#include "xtc.h"		/* the only xtc header included */

/* munit.h pulls a few standard headers; the *contract* is that
 * we needed nothing else from xtc.  Keep this include below
 * "xtc.h" so a missing transitive standard include in xtc.h
 * surfaces as a build error here. */
#include "munit.h"

static MunitResult
test_only_one_header(const MunitParameter params[], void *data)
{
	const char *v, *e;
	int maj, min, pat, rc;
	(void)params; (void)data;

	v = xtc_version_string();
	munit_assert_not_null(v);

	rc = xtc_version_components(&maj, &min, &pat);
	munit_assert_int(rc, ==, XTC_OK);

	e = xtc_strerror(XTC_OK);
	munit_assert_not_null(e);
	munit_assert_string_equal(e, "ok");

	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/c4_one_header", test_only_one_header,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m0/header", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
