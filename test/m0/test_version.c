/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m0/test_version.c
 *	Verifies M0_CLAIMS.md [C1, C2, C3].
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "munit.h"
#include "xtc.h"

/*
 * Hand-rolled SemVer 2.0 matcher.  We deliberately do not link
 * regex(3) here — the contract is so simple that pulling in a
 * dependency would be wasteful, and we want this test to compile
 * with a maximally minimal toolchain.
 */
static int
is_semver(const char *s)
{
	const char *p;
	int seen_digit, dots;

	if (s == NULL || *s == '\0') return 0;
	dots = 0;
	seen_digit = 0;
	p = s;
	for (; *p; p++) {
		if (isdigit((unsigned char)*p)) {
			seen_digit = 1;
			continue;
		}
		if (*p == '.') {
			if (!seen_digit) return 0;
			seen_digit = 0;
			if (++dots > 2) return 0;
			continue;
		}
		break;
	}
	if (dots != 2 || !seen_digit) return 0;
	if (*p == '\0') return 1;
	if (*p != '-') return 0;
	p++;
	if (*p == '\0') return 0;
	for (; *p; p++) {
		if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_'))
			return 0;
	}
	return 1;
}

/* [C1] */
static MunitResult
test_version_string(const MunitParameter params[], void *data)
{
	const char *v;
	(void)params; (void)data;

	v = xtc_version_string();
	munit_assert_not_null(v);
	munit_assert_true(is_semver(v));
	return MUNIT_OK;
}

/* [C2] */
static MunitResult
test_version_components(const MunitParameter params[], void *data)
{
	int major, minor, patch, rc;
	(void)params; (void)data;

	major = minor = patch = -1;
	rc = xtc_version_components(&major, &minor, &patch);
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_int(major, >=, 0);
	munit_assert_int(minor, >=, 0);
	munit_assert_int(patch, >=, 0);

	/* NULL out-parameter must be rejected. */
	rc = xtc_version_components(NULL, &minor, &patch);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	rc = xtc_version_components(&major, NULL, &patch);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	rc = xtc_version_components(&major, &minor, NULL);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [C3]: the string starts with the components. */
static MunitResult
test_version_consistency(const MunitParameter params[], void *data)
{
	int major, minor, patch;
	const char *v;
	char buf[64];
	(void)params; (void)data;

	munit_assert_int(xtc_version_components(&major, &minor, &patch),
	    ==, XTC_OK);
	v = xtc_version_string();
	(void)snprintf(buf, sizeof buf, "%d.%d.%d", major, minor, patch);
	munit_assert_int(strncmp(v, buf, strlen(buf)), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/c1_version_string",     test_version_string,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c2_version_components", test_version_components,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c3_version_consistency",test_version_consistency,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m0/version", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
