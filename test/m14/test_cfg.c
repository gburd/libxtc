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

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(_WIN32)
#include <pthread.h>          /* the concurrent setter in get_string_copy */
#include <stdatomic.h>
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_fs.h"
#include "xtc_cfg.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/* ---- register / duplicate / unregister / count / kind ---- */
static MunitResult
test_register_basic(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = { 0 };
	xtc_cfg_kind_t k;
	xtc_cfg_session_t *ss = NULL;
	xtc_cfg_source_t src;
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

	/* Duplicate name rejected -- with XTC_E_INVAL, the documented code
	 * (xtc_cfg(3) once cited an XTC_E_EXIST that xtc.h never had). */
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_count(), ==, after);
	/* An unset session source is XTC_CFG_SRC_DEFAULT (0).  Sequenced
	 * read on purpose: the probe that reported 99 here printed src in
	 * the same call's argument list, i.e. before the call wrote it. */
	src = (xtc_cfg_source_t)99;
	munit_assert_int(xtc_cfg_session_create(&ss), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_source(ss, "t.reg.int", &src),
	    ==, XTC_OK);
	munit_assert_int(src, ==, XTC_CFG_SRC_DEFAULT);
	src = (xtc_cfg_source_t)99;
	munit_assert_int(xtc_cfg_session_source(ss, "t.reg.nope", &src),
	    ==, XTC_E_NOTFOUND);
	munit_assert_int(src, ==, 99);
	xtc_cfg_session_destroy(ss);

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
	char tmpdir[512], path[600];
	int fd, iv, b, ev;
	int64_t i64;
	double dv;
	const char *sv;
	FILE *f;
	(void)p; (void)d;
	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_cfg_test_XXXXXX", tmpdir);

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
	char tmpdir[512], path[600];
	int fd, ev;
	FILE *f;
	(void)p; (void)d;
	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_cfg_enum_XXXXXX", tmpdir);

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
	char tmpdir[512], path[600];
	int fd, b, iv;
	const char *sv;
	FILE *f;
	(void)p; (void)d;
	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_cfg_parse_XXXXXX", tmpdir);

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

/* ---- kind mismatch: every typed get/set must REJECT a wrong-kind name ----
 *
 * Each typed getter and setter is generated from DEF_GET / DEF_SET_NUM and
 * gates on `v && v->kind == K`.  The existing tests only ever call the
 * accessor matching a variable's real kind, so the FOUND-BUT-WRONG-KIND arm
 * of all ten macro expansions was never taken -- the branch that stops
 * xtc_cfg_get_int from reinterpreting a double's bytes as an int.  That is
 * the whole point of a typed registry, and it was untested.
 *
 * One STRING variable serves as the wrong kind for every numeric accessor,
 * and one INT variable as the wrong kind for the string accessors, so the
 * pair covers both the macro-generated and the hand-written ones.
 */
static MunitResult
test_kind_mismatch(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	const char *sv;
	int64_t i64;
	double dv;
	int iv;
	(void)p; (void)d;

	s.name = "k.str"; s.kind = XTC_CFG_STRING; s.dflt.d_string = "hello";
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "k.int"; s.kind = XTC_CFG_INT; s.dflt.d_int = 7;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	/* Numeric getters on a STRING: found, wrong kind -> XTC_E_INVAL, and
	 * the out-param is left untouched (no partial write). */
	iv = -111; i64 = -111; dv = -111.0;
	munit_assert_int(xtc_cfg_get_bool("k.str", &iv),   ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int("k.str", &iv),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_enum("k.str", &iv),   ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int64("k.str", &i64), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_double("k.str", &dv), ==, XTC_E_INVAL);
	munit_assert_int(iv, ==, -111);
	munit_assert_int64(i64, ==, -111);
	munit_assert_double(dv, ==, -111.0);

	/* Numeric setters on a STRING: rejected, and the string is unharmed. */
	munit_assert_int(xtc_cfg_set_bool("k.str", 1),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int("k.str", 1),      ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_enum("k.str", 0),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int64("k.str", 1),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_double("k.str", 1.0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string("k.str", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "hello");

	/* And the mirror: string accessors on an INT. */
	sv = NULL;
	munit_assert_int(xtc_cfg_get_string("k.int", &sv), ==, XTC_E_INVAL);
	munit_assert_null(sv);
	munit_assert_int(xtc_cfg_set_string("k.int", "nope"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int("k.int", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 7);   /* untouched */

	munit_assert_int(xtc_cfg_unregister("k.str"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("k.int"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- unknown name + NULL args on every accessor ----
 *
 * The NOT-FOUND arm (v == NULL) is the other half of the same gate, and the
 * NULL-argument guards are the first line of each accessor.  Cheap to cover,
 * and they are the arms a typo'd config key hits first.
 */
static MunitResult
test_unknown_and_null(const MunitParameter p[], void *d)
{
	xtc_cfg_kind_t k;
	const char *sv;
	int64_t i64;
	double dv;
	int iv;
	(void)p; (void)d;

	/* Unknown name: every getter and setter reports it, none crash. */
	munit_assert_int(xtc_cfg_get_bool("no.such", &iv),   ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int("no.such", &iv),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int64("no.such", &i64), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_double("no.such", &dv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_enum("no.such", &iv),   ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string("no.such", &sv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_bool("no.such", 0),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int("no.such", 0),      ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int64("no.such", 0),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_double("no.such", 0.0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_enum("no.such", 0),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_string("no.such", "v"), ==, XTC_E_INVAL);
	/* NOTE: kind/unregister report XTC_E_INVAL for an unknown name, not
	 * XTC_E_NOTFOUND -- verified against src/ptc/cfg.c, which initialises
	 * rc = XTC_E_INVAL and only overwrites it on a hit.  Asserted as-is so
	 * this test pins the ACTUAL contract; do not "fix" it to NOTFOUND
	 * without changing the implementation and the header docs together. */
	munit_assert_int(xtc_cfg_kind("no.such", &k),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister("no.such"),   ==, XTC_E_INVAL);

	/* NULL name / NULL out on each accessor. */
	munit_assert_int(xtc_cfg_get_bool(NULL, &iv),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int(NULL, &iv),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int64(NULL, &i64),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_double(NULL, &dv),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_enum(NULL, &iv),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string(NULL, &sv),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_bool("x", NULL),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int("x", NULL),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_int64("x", NULL),   ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_double("x", NULL),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_enum("x", NULL),    ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string("x", NULL),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_bool(NULL, 0),      ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int(NULL, 0),       ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_int64(NULL, 0),     ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_double(NULL, 0.0),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_enum(NULL, 0),      ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_string(NULL, "v"),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_set_string("x", NULL),  ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_kind(NULL, &k),         ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_kind("x", NULL),        ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister(NULL),       ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ---- unbounded numerics: the min==0 && max==0 "no bounds" short-circuit ----
 *
 * __bounds_int_ok / __bounds_dbl_ok treat an all-zero range as "unbounded"
 * and return early.  The existing tests register explicit bounds, so the
 * RANGE-CHECKING half ran while the short-circuit -- the default for any spec
 * that omits min/max, i.e. most real specs -- did not.  Extreme values must
 * be accepted, which is exactly what distinguishes "no bounds" from
 * "bounds [0,0]": a [0,0] range would reject every negative.
 */
static MunitResult
test_unbounded(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	int64_t i64;
	double dv;
	int iv;
	(void)p; (void)d;

	/* min/max left 0 => unbounded. */
	s.name = "u.int"; s.kind = XTC_CFG_INT;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "u.i64"; s.kind = XTC_CFG_INT64;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "u.dbl"; s.kind = XTC_CFG_DOUBLE;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	/* Extremes accepted in BOTH directions -- a [0,0] range would reject
	 * the negatives, so this asserts the short-circuit really was taken. */
	munit_assert_int(xtc_cfg_set_int("u.int", INT_MAX), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("u.int", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, INT_MAX);
	munit_assert_int(xtc_cfg_set_int("u.int", INT_MIN), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("u.int", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, INT_MIN);

	munit_assert_int(xtc_cfg_set_int64("u.i64", INT64_MAX), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int64("u.i64", &i64), ==, XTC_OK);
	munit_assert_int64(i64, ==, INT64_MAX);
	munit_assert_int(xtc_cfg_set_int64("u.i64", INT64_MIN), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int64("u.i64", &i64), ==, XTC_OK);
	munit_assert_int64(i64, ==, INT64_MIN);

	munit_assert_int(xtc_cfg_set_double("u.dbl", -1e300), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_double("u.dbl", &dv), ==, XTC_OK);
	munit_assert_double(dv, ==, -1e300);

	/* Bool's check is a literal 0-or-1 test, not a bounds range: 2 is out. */
	memset(&s, 0, sizeof s);
	s.name = "u.bool"; s.kind = XTC_CFG_BOOL;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_set_bool("u.bool", 2),  ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_bool("u.bool", -1), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_bool("u.bool", 1),  ==, XTC_OK);

	/* Enum bounds come from n_enum_labels, not min/max. */
	memset(&s, 0, sizeof s);
	s.name = "u.enum"; s.kind = XTC_CFG_ENUM;
	s.enum_labels = g_levels; s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_set_enum("u.enum", 3),  ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_enum("u.enum", -1), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_enum("u.enum", 2),  ==, XTC_OK);

	munit_assert_int(xtc_cfg_unregister("u.int"),  ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("u.i64"),  ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("u.dbl"),  ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("u.bool"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("u.enum"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- hot-path read handles (xtc_cfg_ref) ---- */
static MunitResult
test_ref(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	static const char *const lv[] = { "a", "b", "c", NULL };
	xtc_cfg_ref_t ri, rs, re, rb;
	int iv, ev, bv;
	const char *sv;
	(void)p; (void)d;

	/* NULL args rejected; unknown name -> NOTFOUND. */
	munit_assert_int(xtc_cfg_ref(NULL, &ri), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ref("r.int", NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ref("r.nope", &ri), ==, XTC_E_NOTFOUND);
	munit_assert_int(xtc_cfg_ref_get_int(NULL, &iv), ==, XTC_E_INVAL);

	s.name = "r.int"; s.kind = XTC_CFG_INT; s.dflt.d_int = 42;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "r.str"; s.kind = XTC_CFG_STRING; s.dflt.d_string = "hi";
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "r.enum"; s.kind = XTC_CFG_ENUM;
	s.enum_labels = lv; s.n_enum_labels = 3; s.dflt.d_enum = 1;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "r.bool"; s.kind = XTC_CFG_BOOL; s.dflt.d_bool = 1;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	munit_assert_int(xtc_cfg_ref("r.int", &ri), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref("r.str", &rs), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref("r.enum", &re), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref("r.bool", &rb), ==, XTC_OK);

	/* Handle read == default == name-keyed read. */
	munit_assert_int(xtc_cfg_ref_get_int(ri, &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 42);
	munit_assert_int(xtc_cfg_ref_get_string(rs, &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "hi");
	munit_assert_int(xtc_cfg_ref_get_enum(re, &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 1);
	munit_assert_int(xtc_cfg_ref_get_bool(rb, &bv), ==, XTC_OK);
	munit_assert_int(bv, ==, 1);

	/* A live set_* is observed through the SAME handle (no re-ref). */
	munit_assert_int(xtc_cfg_set_int("r.int", 100), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref_get_int(ri, &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 100);
	munit_assert_int(xtc_cfg_set_string("r.str", "bye"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ref_get_string(rs, &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "bye");

	/* Kind mismatch through a handle is rejected, like the name getters. */
	munit_assert_int(xtc_cfg_ref_get_string(ri, &sv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ref_get_int(rs, &iv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ref_get_double(ri, NULL), ==, XTC_E_INVAL);

	munit_assert_int(xtc_cfg_unregister("r.int"),  ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("r.str"),  ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("r.enum"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("r.bool"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- per-session scoping + the transactional override stack ---- */
static MunitResult
test_session_scoping(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = { 0 };
	xtc_cfg_session_t *a = NULL, *b = NULL;
	xtc_cfg_source_t src;
	int v;
	const char *sv;
	(void)p; (void)d;

	/* A global int knob and a global string knob. */
	spec.name = "s.work_mem"; spec.kind = XTC_CFG_INT;
	spec.dflt.d_int = 4096; spec.min_int = 64; spec.max_int = 1000000;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	memset(&spec, 0, sizeof spec);
	spec.name = "s.tz"; spec.kind = XTC_CFG_STRING;
	spec.dflt.d_string = "UTC";
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);

	munit_assert_int(xtc_cfg_session_create(&a), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&b), ==, XTC_OK);

	/* (1) Per-session values with fallback.  Unbound -> global. */
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 4096);

	/* Bind A, SET; the bare getter now sees A's value; B and the global
	 * value are untouched. */
	munit_assert_ptr_null(xtc_cfg_session_bind(a));
	munit_assert_int(xtc_cfg_ssn_set_int(NULL, "s.work_mem", 65536,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 65536);

	/* Switch to B: no override -> falls back to the global default. */
	(void)xtc_cfg_session_bind(b);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 4096);

	/* Unbind: global again, and the global value was never written. */
	(void)xtc_cfg_session_bind(NULL);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 4096);

	/* (2) Bounds/validation apply to session sets too -- and out of
	 * bounds is XTC_E_RANGE, the same code the global setter returns
	 * (pre-1.50 the session path returned XTC_E_INVAL). */
	(void)xtc_cfg_session_bind(a);
	munit_assert_int(xtc_cfg_ssn_set_int(NULL, "s.work_mem", 1,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);   /* below min */

	/* (3) Source precedence: a LOWER-ranked source cannot clobber a
	 * higher one at the same level; an equal-or-higher one can. */
	munit_assert_int(xtc_cfg_ssn_set_int(NULL, "s.work_mem", 8192,
	    XTC_CFG_SRC_FILE), ==, XTC_OK);       /* FILE < SESSION: refused */
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 65536);           /* SESSION value still wins */
	munit_assert_int(xtc_cfg_ssn_set_int(NULL, "s.work_mem", 131072,
	    XTC_CFG_SRC_OVERRIDE), ==, XTC_OK);   /* OVERRIDE > SESSION: wins */
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 131072);
	munit_assert_int(xtc_cfg_session_source(NULL, "s.work_mem", &src),
	    ==, XTC_OK);
	munit_assert_int(src, ==, XTC_CFG_SRC_OVERRIDE);

	/* (4) Transactional stack: a value set inside a pushed level that
	 * ABORTs reverts; one that COMMITs survives. */
	munit_assert_int(xtc_cfg_session_push(NULL), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_int(NULL, "s.work_mem", 262144,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 262144);          /* visible within the level */
	munit_assert_int(xtc_cfg_session_abort(NULL), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 131072);          /* reverted to pre-push */

	munit_assert_int(xtc_cfg_session_push(NULL), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_int(NULL, "s.work_mem", 524288,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_commit(NULL), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 524288);          /* survived the commit */

	/* commit/abort of the base level is rejected (always one level). */
	munit_assert_int(xtc_cfg_session_commit(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_session_abort(NULL),  ==, XTC_E_INVAL);

	/* (5) RESET drops the current-level override -> global shows through. */
	munit_assert_int(xtc_cfg_session_reset(NULL, "s.work_mem"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("s.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 4096);            /* back to global default */

	/* (6) String overrides are per-session and freed on destroy. */
	munit_assert_int(xtc_cfg_ssn_set_string(NULL, "s.tz", "America/New_York",
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_string("s.tz", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "America/New_York");
	(void)xtc_cfg_session_bind(NULL);
	munit_assert_int(xtc_cfg_get_string("s.tz", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "UTC");     /* global unaffected */

	xtc_cfg_session_destroy(a);
	xtc_cfg_session_destroy(b);
	munit_assert_int(xtc_cfg_unregister("s.work_mem"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("s.tz"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- the binding is PER-FIBER, and survives a yield ----
 *
 * REGRESSION (was: a plain _Thread_local, i.e. per-OS-THREAD).  Two
 * fibers on ONE loop, each binding its own session and then YIELDING,
 * read each other's values: fiber A bound 11, yielded; fiber B bound
 * 22; A resumed and read 22.  The yield is the whole point -- the
 * pre-existing session test never yielded between binds, which is why
 * this shipped.  Every assertion below is a read taken AFTER at least
 * one suspension point.
 */
struct fiber_ssn_arg {
	xtc_cfg_session_t *mine;
	int                expect;   /* value our session overrides to */
	int                failed;   /* set by the fiber on any mismatch */
	int                ran;
};

static void
fiber_ssn_body(void *a)
{
	struct fiber_ssn_arg *arg = a;
	int v = -1;

	if (xtc_cfg_session_bind(arg->mine) != NULL) arg->failed = 1;
	/* Yield: the peer fiber runs and binds ITS session here. */
	if (xtc_proc_sleep(5 * 1000 * 1000) != XTC_OK) arg->failed = 1;
	if (xtc_cfg_session_current() != arg->mine) arg->failed = 1;
	if (xtc_cfg_get_int("fs.knob", &v) != XTC_OK || v != arg->expect)
		arg->failed = 1;
	/* A session SET after the yield lands on our own session, and the
	 * peer's later reads must not see it. */
	if (xtc_cfg_ssn_set_int(NULL, "fs.knob", arg->expect + 1,
	    XTC_CFG_SRC_OVERRIDE) != XTC_OK) arg->failed = 1;
	if (xtc_proc_sleep(5 * 1000 * 1000) != XTC_OK) arg->failed = 1;
	if (xtc_cfg_get_int("fs.knob", &v) != XTC_OK || v != arg->expect + 1)
		arg->failed = 1;
	(void)xtc_cfg_session_bind(NULL);
	if (xtc_cfg_session_current() != NULL) arg->failed = 1;
	/* Unbound: the global value shows through again. */
	if (xtc_cfg_get_int("fs.knob", &v) != XTC_OK || v != 7) arg->failed = 1;
	arg->ran = 1;
}

static MunitResult
test_session_per_fiber(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = { 0 };
	xtc_cfg_session_t *a = NULL, *b = NULL;
	struct fiber_ssn_arg aa = { 0 }, ba = { 0 };
	xtc_proc_opts_t opts = { 0 };
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	int v;
	(void)p; (void)d;

	spec.name = "fs.knob"; spec.kind = XTC_CFG_INT; spec.dflt.d_int = 7;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&a), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&b), ==, XTC_OK);
	/* Seed each session's override from OUTSIDE any fiber (explicit
	 * session argument, no binding involved). */
	munit_assert_int(xtc_cfg_ssn_set_int(a, "fs.knob", 11,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_int(b, "fs.knob", 22,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);

	aa.mine = a; aa.expect = 11;
	ba.mine = b; ba.expect = 22;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fiber_ssn_body, &aa, &opts, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fiber_ssn_body, &ba, &opts, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(aa.ran, ==, 1);
	munit_assert_int(ba.ran, ==, 1);
	munit_assert_int(aa.failed, ==, 0);
	munit_assert_int(ba.failed, ==, 0);

	/* The global value was never touched by any session set. */
	munit_assert_int(xtc_cfg_get_int("fs.knob", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 7);

	xtc_cfg_session_destroy(a);
	xtc_cfg_session_destroy(b);
	munit_assert_int(xtc_cfg_unregister("fs.knob"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- unregister vs. session teardown, in EITHER order ----
 *
 * REGRESSION (was: heap-use-after-free).  A session override keeps a raw
 * pointer to the registry entry, and xtc_cfg_unregister freed that entry
 * outright; the later override teardown read entry->kind to decide
 * whether to free a string value.  ASan caught it in __cfg_ovr_free.
 * Both orderings must be safe, and an unregistered name must stop
 * resolving immediately even though a session still overrides it.
 */
static MunitResult
test_unregister_ordering(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = { 0 };
	xtc_cfg_session_t *s = NULL;
	const char *sv;
	int v;
	(void)p; (void)d;

	/* (1) unregister THEN destroy the session.  STRING kind: the free
	 * path is the one that dereferenced the dead entry. */
	spec.name = "u.tz"; spec.kind = XTC_CFG_STRING;
	spec.dflt.d_string = "UTC";
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_string(s, "u.tz", "America/New_York",
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	/* A pushed level with its own override too, so teardown walks more
	 * than one level holding the entry. */
	munit_assert_int(xtc_cfg_session_push(s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_string(s, "u.tz", "Europe/Berlin",
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("u.tz"), ==, XTC_OK);
	/* Gone from the registry immediately, session override or not. */
	munit_assert_int(xtc_cfg_get_string("u.tz", &sv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_unregister("u.tz"), ==, XTC_E_INVAL);
	(void)xtc_cfg_session_bind(s);
	munit_assert_int(xtc_cfg_get_string("u.tz", &sv), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_session_source(s, "u.tz", NULL), ==,
	    XTC_E_INVAL);
	(void)xtc_cfg_session_bind(NULL);
	xtc_cfg_session_destroy(s);   /* was the use-after-free */

	/* (2) the reverse order still works: destroy THEN unregister. */
	s = NULL;
	memset(&spec, 0, sizeof spec);
	spec.name = "u.mem"; spec.kind = XTC_CFG_INT; spec.dflt.d_int = 64;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_int(s, "u.mem", 128,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	xtc_cfg_session_destroy(s);
	munit_assert_int(xtc_cfg_get_int("u.mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 64);
	munit_assert_int(xtc_cfg_unregister("u.mem"), ==, XTC_OK);

	/* (3) an unregister while a session is BOUND, then a RESET of the
	 * dead name, then destroy: reset must not resurrect or double-free. */
	s = NULL;
	memset(&spec, 0, sizeof spec);
	spec.name = "u.str2"; spec.kind = XTC_CFG_STRING;
	spec.dflt.d_string = "g";
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&s), ==, XTC_OK);
	(void)xtc_cfg_session_bind(s);
	munit_assert_int(xtc_cfg_ssn_set_string(NULL, "u.str2", "sessval",
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("u.str2"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_reset(NULL, "u.str2"), ==,
	    XTC_E_NOTFOUND);
	(void)xtc_cfg_session_bind(NULL);
	xtc_cfg_session_destroy(s);
	return MUNIT_OK;
}

/* ---- config-file integer values out of range are REJECTED ----
 *
 * REGRESSION (was: silent wrap through the bounds check).  The INT case
 * parsed with strtol and cast to int with no range check, so on LP64
 * 4294967297 narrowed to 1 and a knob bounded [1,100] ACCEPTED it.  The
 * numeric-enum-index path had the same unchecked narrowing.
 */
static MunitResult
test_load_int_range(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	char tmpdir[512], path[600];
	int fd, iv, ev;
	FILE *f;
	(void)p; (void)d;
	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_cfg_range_XXXXXX", tmpdir);

	s.name = "g.int"; s.kind = XTC_CFG_INT;
	s.dflt.d_int = 3; s.min_int = 1; s.max_int = 100;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "g.free"; s.kind = XTC_CFG_INT; s.dflt.d_int = 5;  /* unbounded */
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "g.lvl"; s.kind = XTC_CFG_ENUM;
	s.enum_labels = g_levels; s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	fd = mkstemp(path);
	munit_assert_int(fd, >=, 0);
	f = fdopen(fd, "w");
	munit_assert_not_null(f);
	fprintf(f,
	    "g.int = 4294967297\n"      /* 2^32+1: wrapped to 1, inside [1,100] */
	    "g.int = -4294967295\n"     /* wrapped to 1 as well */
	    "g.int = 99999999999999\n"  /* > LONG_MAX on ILP32: errno path */
	    "g.free = 2147483648\n"     /* INT_MAX+1 on an UNBOUNDED int knob */
	    "g.free = -2147483649\n"    /* INT_MIN-1 */
	    "g.lvl = 4294967297\n"      /* numeric enum index, wraps to 1 */
	    "g.int = 50\n");            /* the one legitimate line */
	fclose(f);

	/* Exactly one line applies: every out-of-range value is skipped
	 * like any other invalid value. */
	munit_assert_int(xtc_cfg_load_file(path), ==, 1);
	munit_assert_int(xtc_cfg_get_int("g.int", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 50);
	munit_assert_int(xtc_cfg_get_int("g.free", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 5);    /* default: nothing applied */
	munit_assert_int(xtc_cfg_get_enum("g.lvl", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 0);    /* default: nothing applied */

	/* In-range boundary values still apply (the check is inclusive).
	 * fopen(..., "w") truncates, so no truncate(2) call is needed --
	 * and POSIX truncate() does not exist on MSVC, where the implicit
	 * declaration became C4013 and /WX turned it into a build error. */
	f = fopen(path, "w");
	munit_assert_not_null(f);
	fprintf(f, "g.free = 2147483647\ng.lvl = 2\n");
	fclose(f);
	munit_assert_int(xtc_cfg_load_file(path), ==, 2);
	munit_assert_int(xtc_cfg_get_int("g.free", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 2147483647);
	munit_assert_int(xtc_cfg_get_enum("g.lvl", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 2);

	(void)unlink(path);
	munit_assert_int(xtc_cfg_unregister("g.int"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("g.free"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("g.lvl"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- out of bounds is ONE code on BOTH the global and session paths ----
 *
 * PLAN 19.27.13.  The global setters returned XTC_E_RANGE for an
 * out-of-bounds value while the session setters returned XTC_E_INVAL for
 * the same condition.  Every numeric kind, both paths, plus the control
 * that a validator rejection is still XTC_E_INVAL on both.
 */
static MunitResult
test_range_code_consistent(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	xtc_cfg_session_t *ss = NULL;
	int v;
	(void)p; (void)d;

	s.name = "rc.int"; s.kind = XTC_CFG_INT; s.dflt.d_int = 5;
	s.min_int = 0; s.max_int = 10;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "rc.i64"; s.kind = XTC_CFG_INT64; s.dflt.d_int64 = 5;
	s.min_int = 0; s.max_int = 10;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "rc.dbl"; s.kind = XTC_CFG_DOUBLE; s.dflt.d_double = 1.0;
	s.min_double = 0.0; s.max_double = 2.0;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "rc.bool"; s.kind = XTC_CFG_BOOL;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "rc.enum"; s.kind = XTC_CFG_ENUM;
	s.enum_labels = g_levels; s.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "rc.even"; s.kind = XTC_CFG_INT; s.dflt.d_int = 2;
	s.min_int = 0; s.max_int = 100; s.validator = even_only;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	munit_assert_int(xtc_cfg_session_create(&ss), ==, XTC_OK);

	/* Global path (unchanged). */
	munit_assert_int(xtc_cfg_set_int("rc.int", 11), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_int64("rc.i64", -1), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_double("rc.dbl", 2.5), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_bool("rc.bool", 2), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_set_enum("rc.enum", 3), ==, XTC_E_RANGE);
	/* Session path: THE REGRESSION -- these were XTC_E_INVAL. */
	munit_assert_int(xtc_cfg_ssn_set_int(ss, "rc.int", 11,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_ssn_set_int64(ss, "rc.i64", -1,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_ssn_set_double(ss, "rc.dbl", 2.5,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_ssn_set_bool(ss, "rc.bool", 2,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_ssn_set_enum(ss, "rc.enum", 3,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_RANGE);
	/* Control: a validator rejection is INVAL on both paths, and an
	 * in-bounds session set still applies. */
	munit_assert_int(xtc_cfg_set_int("rc.even", 3), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ssn_set_int(ss, "rc.even", 3,
	    XTC_CFG_SRC_SESSION), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_ssn_set_int(ss, "rc.int", 7,
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_ptr_null(xtc_cfg_session_bind(ss));
	munit_assert_int(xtc_cfg_get_int("rc.int", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 7);
	(void)xtc_cfg_session_bind(NULL);

	xtc_cfg_session_destroy(ss);
	munit_assert_int(xtc_cfg_unregister("rc.int"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("rc.i64"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("rc.dbl"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("rc.bool"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("rc.enum"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("rc.even"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- xtc_cfg_get_string_copy: a whole value, safe under a concurrent set
 *
 * PLAN 19.27.13.  xtc_cfg_get_string lends the registry's own buffer and
 * xtc_cfg_set_string frees it, so a reader that copies after the lock
 * drops can read freed memory.  The copying getter must return one whole
 * value -- never a torn or freed one -- while another thread keeps
 * setting the key.  The two values differ in length AND fill byte, so a
 * torn or stale copy fails the shape check (and ASan catches a UAF).
 */
#define GSC_N 256
static char g_gsc_a[GSC_N + 1], g_gsc_b[GSC_N / 2 + 1];

static int
gsc_shape_ok(const char *s)
{
	size_t n = strlen(s), i;
	char c = s[0];
	if (!((c == 'a' && n == GSC_N) || (c == 'b' && n == GSC_N / 2)))
		return 0;
	for (i = 0; i < n; i++)
		if (s[i] != c) return 0;
	return 1;
}

#if !defined(_WIN32)
static _Atomic int g_gsc_stop;

static void *
gsc_setter(void *arg)
{
	unsigned i = 0;
	(void)arg;
	while (!atomic_load(&g_gsc_stop))
		(void)xtc_cfg_set_string("gsc.str", (i++ & 1) ? g_gsc_a : g_gsc_b);
	return NULL;
}
#endif

static MunitResult
test_get_string_copy(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t s = { 0 };
	xtc_cfg_session_t *ss = NULL;
	char *cp = NULL;
#if !defined(_WIN32)
	int i, bad = 0;     /* used only by the POSIX concurrent-set half */
	pthread_t th;
#endif
	(void)p; (void)d;

	memset(g_gsc_a, 'a', GSC_N);
	memset(g_gsc_b, 'b', GSC_N / 2);
	s.name = "gsc.str"; s.kind = XTC_CFG_STRING; s.dflt.d_string = g_gsc_a;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "gsc.null"; s.kind = XTC_CFG_STRING;   /* NULL default */
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	s.name = "gsc.int"; s.kind = XTC_CFG_INT;
	munit_assert_int(xtc_cfg_register(&s), ==, XTC_OK);

	/* Arguments, unknown name, kind mismatch: same codes as get_string. */
	munit_assert_int(xtc_cfg_get_string_copy(NULL, &cp), ==, XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string_copy("gsc.str", NULL), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string_copy("gsc.nope", &cp), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_cfg_get_string_copy("gsc.int", &cp), ==,
	    XTC_E_INVAL);
	munit_assert_ptr_null(cp);
	/* A NULL value copies as NULL. */
	cp = (char *)1;
	munit_assert_int(xtc_cfg_get_string_copy("gsc.null", &cp), ==, XTC_OK);
	munit_assert_ptr_null(cp);

	/* The copy is the caller's: it survives the next set. */
	munit_assert_int(xtc_cfg_get_string_copy("gsc.str", &cp), ==, XTC_OK);
	munit_assert_int(xtc_cfg_set_string("gsc.str", "other"), ==, XTC_OK);
	munit_assert_int(gsc_shape_ok(cp), ==, 1);
	xtc_free(cp);

	/* Resolves through the bound session like get_string. */
	munit_assert_int(xtc_cfg_session_create(&ss), ==, XTC_OK);
	munit_assert_int(xtc_cfg_ssn_set_string(ss, "gsc.str", "sess",
	    XTC_CFG_SRC_SESSION), ==, XTC_OK);
	munit_assert_ptr_null(xtc_cfg_session_bind(ss));
	munit_assert_int(xtc_cfg_get_string_copy("gsc.str", &cp), ==, XTC_OK);
	munit_assert_string_equal(cp, "sess");
	xtc_free(cp);
	(void)xtc_cfg_session_bind(NULL);
	xtc_cfg_session_destroy(ss);

#if !defined(_WIN32)
	/* Concurrent set: every copy is one whole value. */
	munit_assert_int(xtc_cfg_set_string("gsc.str", g_gsc_a), ==, XTC_OK);
	atomic_store(&g_gsc_stop, 0);
	munit_assert_int(pthread_create(&th, NULL, gsc_setter, NULL), ==, 0);
	for (i = 0; i < 200000; i++) {
		cp = NULL;
		if (xtc_cfg_get_string_copy("gsc.str", &cp) != XTC_OK ||
		    cp == NULL || !gsc_shape_ok(cp))
			bad++;
		xtc_free(cp);
	}
	atomic_store(&g_gsc_stop, 1);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	munit_assert_int(bad, ==, 0);
#endif

	munit_assert_int(xtc_cfg_unregister("gsc.str"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("gsc.null"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_unregister("gsc.int"), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/register_basic",   test_register_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/session_scoping",  test_session_scoping,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/session_per_fiber", test_session_per_fiber,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unreg_ordering",   test_unregister_ordering,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/int_bounds",       test_int_bounds,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/double",           test_double,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bool",             test_bool,                  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/string",           test_string,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/enum",             test_enum,                  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/validator_change", test_validator_and_change,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_file",        test_load_file,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_enum_num",    test_load_enum_numeric,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_parse_var",   test_load_parse_variants,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_int_range",   test_load_int_range,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/kind_mismatch",    test_kind_mismatch,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unknown_null",     test_unknown_and_null,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unbounded",        test_unbounded,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ref",              test_ref,                   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/range_code",       test_range_code_consistent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/get_string_copy",  test_get_string_copy,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m14/cfg", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
