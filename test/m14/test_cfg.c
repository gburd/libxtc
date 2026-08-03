/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_cfg.c -- xtc_cfg typed configuration registry.
 *
 *	The registry is a single global list, so each test uses uniquely
 *	named variables and unregisters them at the end to leave the
 *	global state clean for the next test (and for any other test
 *	binary sharing the process is not a concern: each munit test is a
 *	standalone process).  Coverage target: every typed get/set path
 *	(bool/int/int64/double/string/enum), the bounds (XTC_E_RANGE) and
 *	validator (XTC_E_INVAL) rejections, the on_change callback, count
 *	and kind introspection, duplicate-register rejection, and
 *	config-file load/reload for all six kinds including comments and
 *	quotes.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_cfg.h"

/* ---- register / duplicate / unregister / count / kind ---- */
static MunitResult
test_register_basic(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = { 0 };
	xtc_cfg_kind_t k;
	int before, after;
	(void)p; (void)d;

	/* NULL spec / NULL name rejected. */
	munit_assert_int(xtc_cfg_register(NULL), ==, XTC_E_INVAL);
	spec.kind = XTC_CFG_INT;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_E_INVAL);

	before = xtc_cfg_count();
	munit_assert_int(before, >=, 0);

	spec.name = "t.reg.int";
	spec.short_desc = "an int knob";
	spec.kind = XTC_CFG_INT;
	spec.dflt.d_int = 42;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);

	after = xtc_cfg_count();
	munit_assert_int(after, ==, before + 1);

	/* Duplicate name rejected. */
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_count(), ==, after);

	/* kind introspection. */
	munit_assert_int(xtc_cfg_kind(NULL, &k), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_kind("t.reg.int", NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_kind("t.reg.nope", &k), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_kind("t.reg.int", &k), ==, XTC_OK);
	munit_assert_int(k, ==, XTC_CFG_INT);

	/* Unregister: NULL, unknown, then real. */
	munit_assert_int(xtc_cfg_unregister(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister("t.reg.nope"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister("t.reg.int"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_count(), ==, before);
	return MUNIT_OK;
}

/* ---- int / int64 with bounds ---- */
static MunitResult
test_int_bounds(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	int iv;
	int64_t i64;
	(void)p; (void)d;

	s.name = "t.int.bounded";
	s.kind = XTC_CFG_INT;
	s.dflt.d_int = 5;
	s.min_int = 1;
	s.max_int = 10;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	munit_assert_int(xtc_cfg_get_int("t.int.bounded", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 5);
	/* get with NULL args, wrong type, unknown name. */
	munit_assert_int(xtc_cfg_get_int(NULL, &iv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int("t.int.bounded", NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int("t.int.nope", &iv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int64("t.int.bounded", &i64), ==, XTC_E_INVAL);

	/* set in range, out of range (both ends), NULL name. */
	munit_assert_int(xtc_cfg_set_int(NULL, 3), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int("t.int.bounded", 3), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("t.int.bounded", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 3);
	munit_assert_int(xtc_cfg_set_int("t.int.bounded", 0), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_int("t.int.bounded", 11), ==, XTC_E_RANGE);
	/* wrong-type set rejected. */
	munit_assert_int(xtc_cfg_set_int64("t.int.bounded", 3), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister("t.int.bounded"), ==, XTC_OK);

	/* int64, unbounded (min==max==0). */
	memset(&s, 0, sizeof s);
	s.name = "t.i64";
	s.kind = XTC_CFG_INT64;
	s.dflt.d_int64 = 100;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int64("t.i64", &i64), ==, XTC_OK);
	munit_assert_int64(i64, ==, 100);
	munit_assert_int(xtc_cfg_set_int64("t.i64", 1LL << 40), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int64("t.i64", &i64), ==, XTC_OK);
	munit_assert_int64(i64, ==, 1LL << 40);
	/* int64 bounded RANGE. */
	munit_assert_int(xtc_cfg_unregister("t.i64"), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "t.i64b";
	s.kind = XTC_CFG_INT64;
	s.dflt.d_int64 = 5;
	s.min_int = -3;
	s.max_int = 9;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_set_int64("t.i64b", 10), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_int64("t.i64b", -4), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_int64("t.i64b", -3), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("t.i64b"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- double with bounds ---- */
static MunitResult
test_double(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	double dv;
	(void)p; (void)d;

	s.name = "t.dbl";
	s.kind = XTC_CFG_DOUBLE;
	s.dflt.d_double = 1.5;
	s.min_double = 0.0;      /* min==max==0 means unbounded, so use max */
	s.max_double = 100.0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	munit_assert_int(xtc_cfg_get_double("t.dbl", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, 1.5);
	munit_assert_int(xtc_cfg_get_double(NULL, &dv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_double("t.dbl", NULL), ==, XTC_E_INVAL);

	munit_assert_int(xtc_cfg_set_double(NULL, 2.0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_double("t.dbl", 2.5), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_double("t.dbl", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, 2.5);
	munit_assert_int(xtc_cfg_set_double("t.dbl", 200.0), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_unregister("t.dbl"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- bool ---- */
static MunitResult
test_bool(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	int b;
	(void)p; (void)d;

	s.name = "t.bool";
	s.kind = XTC_CFG_BOOL;
	s.dflt.d_bool = 1;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	munit_assert_int(xtc_cfg_get_bool("t.bool", &b), ==, XTC_OK);
	munit_assert_int(b, ==, 1);
	munit_assert_int(xtc_cfg_set_bool("t.bool", 0), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_bool("t.bool", &b), ==, XTC_OK);
	munit_assert_int(b, ==, 0);
	/* Only 0/1 accepted (bounds_check in DEF_SET_NUM). */
	munit_assert_int(xtc_cfg_set_bool("t.bool", 2), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_unregister("t.bool"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- string ---- */
static MunitResult
test_string(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	const char *sv;
	(void)p; (void)d;

	s.name = "t.str";
	s.kind = XTC_CFG_STRING;
	s.dflt.d_string = "hello";
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	munit_assert_int(xtc_cfg_get_string("t.str", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "hello");
	munit_assert_int(xtc_cfg_get_string(NULL, &sv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string("t.str", NULL), ==, XTC_E_INVAL);

	munit_assert_int(xtc_cfg_set_string(NULL, "x"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_string("t.str", NULL), ==, XTC_E_INVAL);
	/* set twice: the second frees the first copy (exercises free path). */
	munit_assert_int(xtc_cfg_set_string("t.str", "world"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_set_string("t.str", "again"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_string("t.str", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "again");
	/* wrong-type set on a string var. */
	munit_assert_int(xtc_cfg_set_int("t.str", 1), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister("t.str"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- enum ---- */
static const char *const g_levels[] = { "low", "mid", "high", NULL };

static MunitResult
test_enum(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	int ev;
	(void)p; (void)d;

	s.name = "t.enum";
	s.kind = XTC_CFG_ENUM;
	s.dflt.d_enum = 1;
	s.enum_labels = g_levels;
	s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	munit_assert_int(xtc_cfg_get_enum("t.enum", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 1);
	munit_assert_int(xtc_cfg_set_enum("t.enum", 2), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_enum("t.enum", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 2);
	/* out-of-range index rejected. */
	munit_assert_int(xtc_cfg_set_enum("t.enum", 3), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_enum("t.enum", -1), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_unregister("t.enum"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- validator + on_change callback ---- */
static int g_changes;
static int64_t g_last_old, g_last_new;

static int
even_only(const void *new_val, void *user)
{
	const int *v = (const int *)new_val;
	(void)user;
	return (*v % 2 == 0) ? XTC_OK : XTC_E_INVAL;
}

static void
on_change_cb(const char *name, const void *old_val, const void *new_val,
             void *user)
{
	(void)name; (void)user;
	g_changes++;
	g_last_old = *(const int *)old_val;
	g_last_new = *(const int *)new_val;
}

static MunitResult
test_validator_and_change(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	(void)p; (void)d;

	s.name = "t.cb";
	s.kind = XTC_CFG_INT;
	s.dflt.d_int = 2;
	s.validator = even_only;
	s.on_change = on_change_cb;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	g_changes = 0;
	/* Odd rejected by validator; no change callback. */
	munit_assert_int(xtc_cfg_set_int("t.cb", 3), ==, XTC_E_INVAL);
	munit_assert_int(g_changes, ==, 0);
	/* Even accepted; callback fires with old=2 new=8. */
	munit_assert_int(xtc_cfg_set_int("t.cb", 8), ==, XTC_OK);
	munit_assert_int(g_changes, ==, 1);
	munit_assert_int64(g_last_old, ==, 2);
	munit_assert_int64(g_last_new, ==, 8);
	munit_assert_int(xtc_cfg_unregister("t.cb"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- config file load + reload (all kinds, comments, quotes) ---- */
static MunitResult
test_load_file(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	char path[] = "/tmp/xtc_cfg_test_XXXXXX";
	int fd, iv, b, ev;
	int64_t i64;
	double dv;
	const char *sv;
	FILE *f;
	(void)p; (void)d;

	/* Register one var of each kind. */
	s.name = "f.int";    s.kind = XTC_CFG_INT;    s.dflt.d_int = 0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "f.i64";    s.kind = XTC_CFG_INT64;  s.dflt.d_int64 = 0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "f.dbl";    s.kind = XTC_CFG_DOUBLE; s.dflt.d_double = 0.0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "f.bool";   s.kind = XTC_CFG_BOOL;   s.dflt.d_bool = 0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "f.str";    s.kind = XTC_CFG_STRING; s.dflt.d_string = "";
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "f.enum";   s.kind = XTC_CFG_ENUM;
	s.enum_labels = g_levels; s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	/* NULL path, and a nonexistent path (XTC_E_IO). */
	munit_assert_int(xtc_cfg_load_file(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_load_file("/no/such/xtc/cfg/file"), ==,
	    XTC_E_IO);
	/* reload with nothing loaded yet -> XTC_E_INVAL. */
	munit_assert_int(xtc_cfg_reload(), ==, XTC_E_INVAL);

	fd = mkstemp(path);
	munit_assert_int(fd, >=, 0);
	f = fdopen(fd, "w");
	munit_assert_not_null(f);
	fprintf(f,
	    "# a comment line\n"
	    "\n"
	    "f.int = 7\n"
	    "f.i64 = 123456789012\n"
	    "f.dbl = 3.25\n"
	    "f.bool = on\n"
	    "f.str = 'quoted value'\n"
	    "f.enum = high\n"
	    "f.int_trailing = 9 # trailing comment ignored on unknown key\n"
	    "no_equals_line\n"
	    "unknown.key = 1\n"
	    "f.dbl = bad_double\n"       /* malformed double: skipped */
	    "f.i64 = not_a_number\n"     /* malformed int64: skipped */
	    "f.str = \"double quoted\"\n" /* double-quote variant */
	    "f.int = 8 # unquoted trailing comment stripped\n");
	fclose(f);

	/* Applied: f.int(7), f.i64(1), f.dbl(1), f.bool(1), f.str('quoted'),
	 * f.enum(1), f.str("double quoted"), f.int(8) = 8 successful applies
	 * (unknown.key, the malformed double/int64, and the malformed lines
	 * are skipped). */
	munit_assert_int(xtc_cfg_load_file(path), ==, 8);

	munit_assert_int(xtc_cfg_get_int("f.int", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 8);
	munit_assert_int(xtc_cfg_get_int64("f.i64", &i64), ==, XTC_OK);
	munit_assert_int64(i64, ==, 123456789012LL);
	munit_assert_int(xtc_cfg_get_double("f.dbl", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, 3.25);
	munit_assert_int(xtc_cfg_get_bool("f.bool", &b), ==, XTC_OK);
	munit_assert_int(b, ==, 1);
	munit_assert_int(xtc_cfg_get_string("f.str", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "double quoted");
	munit_assert_int(xtc_cfg_get_enum("f.enum", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 2);   /* "high" */

	/* reload re-reads the same path; same applied count. */
	munit_assert_int(xtc_cfg_reload(), ==, 8);

	(void)unlink(path);
	munit_assert_int(xtc_cfg_unregister("f.int"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("f.i64"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("f.dbl"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("f.bool"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("f.str"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("f.enum"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- enum load by numeric index + bad values are skipped ---- */
static MunitResult
test_load_enum_numeric(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	char path[] = "/tmp/xtc_cfg_enum_XXXXXX";
	int fd, ev;
	FILE *f;
	(void)p; (void)d;

	s.name = "e.lvl";
	s.kind = XTC_CFG_ENUM;
	s.enum_labels = g_levels;
	s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	fd = mkstemp(path);
	munit_assert_int(fd, >=, 0);
	f = fdopen(fd, "w");
	munit_assert_not_null(f);
	/* Numeric index accepted; a bogus label skipped. */
	fprintf(f, "e.lvl = bogus\ne.lvl = 2\n");
	fclose(f);

	munit_assert_int(xtc_cfg_load_file(path), ==, 1);
	munit_assert_int(xtc_cfg_get_enum("e.lvl", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 2);

	(void)unlink(path);
	munit_assert_int(xtc_cfg_unregister("e.lvl"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- bool-parse label variants + malformed int, via load_file ---- */
static MunitResult
test_load_parse_variants(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	char path[] = "/tmp/xtc_cfg_parse_XXXXXX";
	int fd, b, iv;
	const char *sv;
	FILE *f;
	(void)p; (void)d;

	s.name = "v.bool"; s.kind = XTC_CFG_BOOL; s.dflt.d_bool = 1;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "v.int"; s.kind = XTC_CFG_INT; s.dflt.d_int = 1;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "v.str"; s.kind = XTC_CFG_STRING; s.dflt.d_string = "x";
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	fd = mkstemp(path);
	munit_assert_int(fd, >=, 0);
	f = fdopen(fd, "w");
	munit_assert_not_null(f);
	/* "false"/"no"/"0" all parse to 0 (the second bool-parse branch);
	 * a garbage bool is rejected; a malformed int is rejected; leading
	 * tabs are trimmed; an empty (unset) value applies the empty string. */
	fprintf(f,
	    "v.bool = false\n"
	    "v.bool = garbage\n"      /* rejected, prior value kept */
	    "\tv.int = notanint\n"   /* leading tab trimmed; value rejected */
	    "v.int = 55\n"
	    "v.str = \n");           /* empty value -> empty string */
	fclose(f);

	/* Applied: v.bool(false), v.int(55), v.str(empty) = 3. */
	munit_assert_int(xtc_cfg_load_file(path), ==, 3);
	munit_assert_int(xtc_cfg_get_bool("v.bool", &b), ==, XTC_OK);
	munit_assert_int(b, ==, 0);
	munit_assert_int(xtc_cfg_get_int("v.int", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 55);
	munit_assert_int(xtc_cfg_get_string("v.str", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "");

	(void)unlink(path);
	munit_assert_int(xtc_cfg_unregister("v.bool"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("v.int"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("v.str"), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/register_basic",   test_register_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/int_bounds",       test_int_bounds,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/double",           test_double,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bool",             test_bool,                  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/string",           test_string,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/enum",             test_enum,                  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/validator_change", test_validator_and_change,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_file",        test_load_file,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_enum_num",    test_load_enum_numeric,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_parse_var",   test_load_parse_variants,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m14/cfg", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
