/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m99/test_resp_parser.c
 *	Table-driven tests for the RESP protocol parser.
 */

#include <string.h>
#include <stdio.h>

#include "munit.h"
#include "../../examples/05_rexis/proto.h"

/* Test case structure */
typedef struct {
	const char *name;
	const char *input;
	size_t      input_len;
	resp_err_t  expected_err;
	resp_type_t expected_type;
	int64_t     expected_int;
	const char *expected_str;
	size_t      expected_str_len;
	size_t      expected_count;
} test_case_t;

/* Helper to create test cases with embedded NULs */
#define TC(n, inp, err, typ, ...) \
	{ .name = n, .input = inp, .input_len = sizeof(inp) - 1, \
	  .expected_err = err, .expected_type = typ, __VA_ARGS__ }

/* ----- Simple string tests ----- */

static MunitResult
test_simple_string(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "+OK\r\n", 5);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_SIMPLE);
	munit_assert_size(val.u.str.len, ==, 2);
	munit_assert_memory_equal(2, val.u.str.data, "OK");
	munit_assert_size(consumed, ==, 5);

	return MUNIT_OK;
}

static MunitResult
test_simple_string_empty(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "+\r\n", 3);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_SIMPLE);
	munit_assert_size(val.u.str.len, ==, 0);

	return MUNIT_OK;
}

/* ----- Error tests ----- */

static MunitResult
test_error(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "-ERR unknown command\r\n", 22);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_ERROR);
	munit_assert_size(val.u.str.len, ==, 19);
	munit_assert_memory_equal(19, val.u.str.data, "ERR unknown command");

	return MUNIT_OK;
}

/* ----- Integer tests ----- */

static MunitResult
test_integer_positive(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, ":1000\r\n", 7);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_INT);
	munit_assert_llong(val.u.ival, ==, 1000);

	return MUNIT_OK;
}

static MunitResult
test_integer_negative(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, ":-42\r\n", 6);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_INT);
	munit_assert_llong(val.u.ival, ==, -42);

	return MUNIT_OK;
}

static MunitResult
test_integer_zero(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, ":0\r\n", 4);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_INT);
	munit_assert_llong(val.u.ival, ==, 0);

	return MUNIT_OK;
}

/* ----- Bulk string tests ----- */

static MunitResult
test_bulk_string(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$5\r\nhello\r\n", 11);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_BULK);
	munit_assert_size(val.u.str.len, ==, 5);
	munit_assert_memory_equal(5, val.u.str.data, "hello");
	munit_assert_size(consumed, ==, 11);

	return MUNIT_OK;
}

static MunitResult
test_bulk_string_empty(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$0\r\n\r\n", 6);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_BULK);
	munit_assert_size(val.u.str.len, ==, 0);

	return MUNIT_OK;
}

static MunitResult
test_bulk_string_null(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$-1\r\n", 5);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_NULL);

	return MUNIT_OK;
}

static MunitResult
test_bulk_string_binary(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	/* Binary data with embedded NUL */
	const char input[] = "$6\r\nhe\x00llo\r\n";
	(void)p; (void)d;

	resp_parser_init(&parser, input, sizeof(input) - 1);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_BULK);
	munit_assert_size(val.u.str.len, ==, 6);
	munit_assert_memory_equal(6, val.u.str.data, "he\x00llo");

	return MUNIT_OK;
}

/* ----- Array tests ----- */

static MunitResult
test_array_empty(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "*0\r\n", 4);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_ARRAY);
	munit_assert_size(val.u.count, ==, 0);

	return MUNIT_OK;
}

static MunitResult
test_array_null(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "*-1\r\n", 5);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_NULL);

	return MUNIT_OK;
}

static MunitResult
test_array_integers(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t vals[4];
	size_t consumed;
	resp_err_t rc;
	int i;
	(void)p; (void)d;

	resp_parser_init(&parser, "*3\r\n:1\r\n:2\r\n:3\r\n", 16);

	rc = resp_parse(&parser, &vals[0], &consumed);
	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(vals[0].type, ==, RESP_TYPE_ARRAY);
	munit_assert_size(vals[0].u.count, ==, 3);

	for (i = 1; i <= 3; i++) {
		rc = resp_parse(&parser, &vals[i], &consumed);
		munit_assert_int(rc, ==, RESP_OK);
		munit_assert_int(vals[i].type, ==, RESP_TYPE_INT);
		munit_assert_llong(vals[i].u.ival, ==, i);
	}

	return MUNIT_OK;
}

/* ----- Command parsing tests ----- */

static MunitResult
test_command_ping(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t argv[10];
	int argc;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	/* *1\r\n$4\r\nPING\r\n */
	resp_parser_init(&parser, "*1\r\n$4\r\nPING\r\n", 14);
	rc = resp_parse_command(&parser, argv, 10, &argc, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(argc, ==, 1);
	munit_assert_int(argv[0].type, ==, RESP_TYPE_BULK);
	munit_assert_size(argv[0].u.str.len, ==, 4);
	munit_assert_memory_equal(4, argv[0].u.str.data, "PING");

	return MUNIT_OK;
}

static MunitResult
test_command_set(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t argv[10];
	int argc;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	/* *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n */
	resp_parser_init(&parser,
	    "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", 32);
	rc = resp_parse_command(&parser, argv, 10, &argc, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(argc, ==, 3);
	munit_assert_memory_equal(3, argv[0].u.str.data, "SET");
	munit_assert_memory_equal(3, argv[1].u.str.data, "foo");
	munit_assert_memory_equal(3, argv[2].u.str.data, "bar");

	return MUNIT_OK;
}

/* ----- Incomplete data tests ----- */

static MunitResult
test_incomplete_simple(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "+OK", 3);  /* missing \r\n */
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_NEED_MORE);

	return MUNIT_OK;
}

static MunitResult
test_incomplete_bulk(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$5\r\nhel", 7);  /* incomplete data */
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_NEED_MORE);

	return MUNIT_OK;
}

static MunitResult
test_incomplete_bulk_no_crlf(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$5\r\nhello", 9);  /* missing trailing \r\n */
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_NEED_MORE);

	return MUNIT_OK;
}

/* ----- Malformed input tests ----- */

static MunitResult
test_malformed_type(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "XBAD\r\n", 6);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_ERR_PROTO);

	return MUNIT_OK;
}

static MunitResult
test_malformed_integer(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, ":abc\r\n", 6);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_ERR_PROTO);

	return MUNIT_OK;
}

static MunitResult
test_malformed_bulk_length(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$abc\r\ndata\r\n", 12);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_ERR_PROTO);

	return MUNIT_OK;
}

static MunitResult
test_malformed_array_count(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "*xyz\r\n", 6);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_ERR_PROTO);

	return MUNIT_OK;
}

static MunitResult
test_malformed_negative_length(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "$-2\r\n", 5);  /* Only -1 is valid for null */
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_ERR_PROTO);

	return MUNIT_OK;
}

/* ----- Response builder tests ----- */

static MunitResult
test_write_simple(const MunitParameter p[], void *d)
{
	char buf[64];
	resp_buf_t out;
	(void)p; (void)d;

	resp_buf_init(&out, buf, sizeof buf);
	resp_write_simple(&out, "OK");

	munit_assert_size(out.len, ==, 5);
	munit_assert_memory_equal(5, buf, "+OK\r\n");

	return MUNIT_OK;
}

static MunitResult
test_write_error(const MunitParameter p[], void *d)
{
	char buf[64];
	resp_buf_t out;
	(void)p; (void)d;

	resp_buf_init(&out, buf, sizeof buf);
	resp_write_error(&out, "not found");

	munit_assert_size(out.len, ==, 15);
	munit_assert_memory_equal(15, buf, "-ERR not found\r\n");

	return MUNIT_OK;
}

static MunitResult
test_write_int(const MunitParameter p[], void *d)
{
	char buf[64];
	resp_buf_t out;
	(void)p; (void)d;

	resp_buf_init(&out, buf, sizeof buf);
	resp_write_int(&out, 12345);

	munit_assert_memory_equal(out.len, buf, ":12345\r\n");

	return MUNIT_OK;
}

static MunitResult
test_write_bulk(const MunitParameter p[], void *d)
{
	char buf[64];
	resp_buf_t out;
	(void)p; (void)d;

	resp_buf_init(&out, buf, sizeof buf);
	resp_write_bulk(&out, "hello", 5);

	munit_assert_size(out.len, ==, 11);
	munit_assert_memory_equal(11, buf, "$5\r\nhello\r\n");

	return MUNIT_OK;
}

static MunitResult
test_write_bulk_null(const MunitParameter p[], void *d)
{
	char buf[64];
	resp_buf_t out;
	(void)p; (void)d;

	resp_buf_init(&out, buf, sizeof buf);
	resp_write_bulk_null(&out);

	munit_assert_size(out.len, ==, 5);
	munit_assert_memory_equal(5, buf, "$-1\r\n");

	return MUNIT_OK;
}

static MunitResult
test_write_array(const MunitParameter p[], void *d)
{
	char buf[128];
	resp_buf_t out;
	(void)p; (void)d;

	resp_buf_init(&out, buf, sizeof buf);
	resp_write_array(&out, 2);
	resp_write_bulk(&out, "foo", 3);
	resp_write_bulk(&out, "bar", 3);

	/* *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n = 4+9+9 = 22 */
	munit_assert_size(out.len, ==, 22);
	munit_assert_memory_equal(4, buf, "*2\r\n");

	return MUNIT_OK;
}

/* ----- RESP3 tests ----- */

static MunitResult
test_resp3_null(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "_\r\n", 3);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_NULL);

	return MUNIT_OK;
}

static MunitResult
test_resp3_boolean_true(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "#t\r\n", 4);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_BOOL);
	munit_assert_int(val.u.bval, ==, 1);

	return MUNIT_OK;
}

static MunitResult
test_resp3_boolean_false(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "#f\r\n", 4);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_BOOL);
	munit_assert_int(val.u.bval, ==, 0);

	return MUNIT_OK;
}

static MunitResult
test_resp3_map(const MunitParameter p[], void *d)
{
	resp_parser_t parser;
	resp_value_t val;
	size_t consumed;
	resp_err_t rc;
	(void)p; (void)d;

	resp_parser_init(&parser, "%2\r\n", 4);
	rc = resp_parse(&parser, &val, &consumed);

	munit_assert_int(rc, ==, RESP_OK);
	munit_assert_int(val.type, ==, RESP_TYPE_MAP);
	munit_assert_size(val.u.count, ==, 2);

	return MUNIT_OK;
}

/* ----- Test suite ----- */

static MunitTest tests[] = {
	/* Simple strings */
	{ "/simple_string",        test_simple_string,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/simple_string_empty",  test_simple_string_empty, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Errors */
	{ "/error",                test_error,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Integers */
	{ "/integer_positive",     test_integer_positive,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/integer_negative",     test_integer_negative,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/integer_zero",         test_integer_zero,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Bulk strings */
	{ "/bulk_string",          test_bulk_string,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bulk_string_empty",    test_bulk_string_empty,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bulk_string_null",     test_bulk_string_null,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bulk_string_binary",   test_bulk_string_binary,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Arrays */
	{ "/array_empty",          test_array_empty,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/array_null",           test_array_null,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/array_integers",       test_array_integers,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Commands */
	{ "/command_ping",         test_command_ping,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/command_set",          test_command_set,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Incomplete */
	{ "/incomplete_simple",    test_incomplete_simple,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/incomplete_bulk",      test_incomplete_bulk,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/incomplete_bulk_no_crlf", test_incomplete_bulk_no_crlf, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Malformed */
	{ "/malformed_type",       test_malformed_type,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/malformed_integer",    test_malformed_integer,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/malformed_bulk_length", test_malformed_bulk_length, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/malformed_array_count", test_malformed_array_count, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/malformed_negative_length", test_malformed_negative_length, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* Response builder */
	{ "/write_simple",         test_write_simple,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/write_error",          test_write_error,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/write_int",            test_write_int,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/write_bulk",           test_write_bulk,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/write_bulk_null",      test_write_bulk_null,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/write_array",          test_write_array,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	/* RESP3 */
	{ "/resp3_null",           test_resp3_null,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/resp3_boolean_true",   test_resp3_boolean_true,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/resp3_boolean_false",  test_resp3_boolean_false, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/resp3_map",            test_resp3_map,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/resp_parser", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
