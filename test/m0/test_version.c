/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m0/test_version.c
 *	Verifies the version API.  The components are the ABI level and match
 *	the XTC_VERSION_MAJOR/_MINOR/_PATCH macros; the string is the release
 *	name and matches XTC_VERSION_STRING.  The release name is either the
 *	ABI level as SemVer (an LTS release, or an untagged tree) or a CalVer
 *	date YYYY.MM.DD[.N] (a development release from main).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "munit.h"
#include "xtc.h"

/*
 * Hand-rolled SemVer 2.0 matcher.  We deliberately do not link
 * regex(3) here -- the contract is so simple that pulling in a
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

/*
 * CalVer YYYY.MM.DD with an optional .N, the form dist/release_name.sh
 * accepts: a year from 2000, a month 01-12, a day 01-31, and N >= 1
 * with no leading zero.
 */
static int
is_calver(const char *s)
{
	int i, mon, day;

	if (s == NULL || strlen(s) < 10)
		return 0;
	for (i = 0; i < 10; i++) {
		if (i == 4 || i == 7) {
			if (s[i] != '.')
				return 0;
		} else if (!isdigit((unsigned char)s[i]))
			return 0;
	}
	if (s[0] != '2' || s[1] != '0')
		return 0;
	mon = (s[5] - '0') * 10 + (s[6] - '0');
	day = (s[8] - '0') * 10 + (s[9] - '0');
	if (mon < 1 || mon > 12 || day < 1 || day > 31)
		return 0;
	if (s[10] == '\0')
		return 1;
	if (s[10] != '.' || s[11] < '1' || s[11] > '9')
		return 0;
	for (i = 12; s[i] != '\0'; i++) {
		if (!isdigit((unsigned char)s[i]))
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
	munit_assert_true(is_semver(v) || is_calver(v));
	munit_assert_string_equal(v, XTC_VERSION_STRING);

	/* The matchers themselves, on the shapes the release tags take. */
	munit_assert_true(is_calver("2026.11.24"));
	munit_assert_true(is_calver("2026.11.24.2"));
	munit_assert_false(is_calver("2026.13.01"));
	munit_assert_false(is_calver("2026.11.24.0"));
	munit_assert_false(is_calver("2.3.0"));
	munit_assert_true(is_semver("2.1.6"));
	munit_assert_false(is_semver("2026.11.24.2"));
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
	munit_assert_int(major, ==, XTC_VERSION_MAJOR);
	munit_assert_int(minor, ==, XTC_VERSION_MINOR);
	munit_assert_int(patch, ==, XTC_VERSION_PATCH);

	/* NULL out-parameter must be rejected. */
	rc = xtc_version_components(NULL, &minor, &patch);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	rc = xtc_version_components(&major, NULL, &patch);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	rc = xtc_version_components(&major, &minor, NULL);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/*
 * [C3]: a SemVer string starts with the components (an LTS release is
 * named by its ABI level).  A CalVer string is a date and says nothing
 * about the ABI level, which [C2] checks against the macros.
 */
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
	if (is_calver(v))
		return MUNIT_OK;
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
