/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sqlxtc/test_quack.c
 *	Quack JSON parser + encoder unit tests.  Standalone, no munit
 *	dependency: built and run by examples/06_sqlxtc/Makefile.
 *
 *	20+ cases.  Reports pass count to stdout; exit 1 on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../examples/06_sqlxtc/quack.h"

static int n_pass = 0;
static int n_fail = 0;

#define CHECK(cond, name) do {                                          \
	if (cond) { n_pass++; printf("  OK   %s\n", name); }            \
	else      { n_fail++; printf("  FAIL %s\n", name); }            \
} while (0)

static int
parse(const char *line, quack_msg_t *m)
{
	return quack_parse(line, strlen(line), m);
}

int
main(void)
{
	quack_msg_t m;

	/* 1: simple query */
	CHECK(parse("{\"q\":\"SELECT 1\"}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.q_len == 8 &&
	      memcmp(m.q, "SELECT 1", 8) == 0,
	      "simple_query");

	/* 2: query with limit */
	CHECK(parse("{\"q\":\"SELECT *\",\"limit\":42}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.limit == 42,
	      "query_with_limit");

	/* 3: ping */
	CHECK(parse("{\"ping\":1}", &m) == 0 && m.kind == QUACK_MSG_PING,
	      "ping");

	/* 4: quit */
	CHECK(parse("{\"quit\":1}", &m) == 0 && m.kind == QUACK_MSG_QUIT,
	      "quit");

	/* 5: empty object => UNKNOWN (well-formed but no recognized key) */
	CHECK(parse("{}", &m) == 0 && m.kind == QUACK_MSG_UNKNOWN,
	      "empty_object");

	/* 6: not an object */
	CHECK(parse("[]", &m) < 0, "not_an_object");

	/* 7: malformed JSON */
	CHECK(parse("{\"q\":}", &m) < 0, "malformed_value");

	/* 8: unterminated string */
	CHECK(parse("{\"q\":\"hello", &m) < 0, "unterminated_string");

	/* 9: unknown key, gracefully skipped */
	CHECK(parse("{\"foo\":1,\"q\":\"X\"}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.q_len == 1 && m.q[0] == 'X',
	      "skip_unknown_key");

	/* 10: whitespace tolerance */
	CHECK(parse("  {  \"q\" : \"X\" }  ", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY,
	      "whitespace_tolerant");

	/* 11: query with embedded escape */
	CHECK(parse("{\"q\":\"line1\\nline2\"}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.q_len == 11 &&
	      memcmp(m.q, "line1\nline2", 11) == 0,
	      "escape_newline");

	/* 12: query with quote escape */
	CHECK(parse("{\"q\":\"a \\\"b\\\" c\"}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.q_len == 7 &&
	      memcmp(m.q, "a \"b\" c", 7) == 0,
	      "escape_quote");

	/* 13: unicode escape */
	CHECK(parse("{\"q\":\"x\\u00e9y\"}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.q_len == 4 &&
	      (unsigned char)m.q[1] == 0xC3 &&
	      (unsigned char)m.q[2] == 0xA9,
	      "escape_unicode");

	/* 14: bad escape rejected */
	CHECK(parse("{\"q\":\"\\x\"}", &m) < 0, "bad_escape");

	/* 15: deeply-nested unknown value gets skipped */
	CHECK(parse("{\"opts\":{\"a\":[1,2,{\"b\":\"c\"}]},\"q\":\"X\"}",
	            &m) == 0 && m.kind == QUACK_MSG_QUERY,
	      "deep_unknown_skipped");

	/* 16: limit with negative */
	CHECK(parse("{\"q\":\"X\",\"limit\":-7}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.limit == -7,
	      "limit_negative");

	/* 17: q must be string */
	CHECK(parse("{\"q\":42}", &m) < 0, "q_not_string");

	/* 18: limit must be int */
	CHECK(parse("{\"q\":\"x\",\"limit\":\"big\"}", &m) < 0,
	      "limit_not_int");

	/* 18b: bound parameters -- int, text, null */
	CHECK(parse("{\"q\":\"INSERT INTO t VALUES(?1,?2,?3)\","
	            "\"params\":[7,\"hello\",null]}", &m) == 0 &&
	      m.kind == QUACK_MSG_QUERY && m.n_params == 3 &&
	      m.params[0].type == QUACK_P_INT && m.params[0].ival == 7 &&
	      m.params[1].type == QUACK_P_TEXT && m.params[1].slen == 5 &&
	      memcmp(m.params[1].sval, "hello", 5) == 0 &&
	      m.params[2].type == QUACK_P_NULL,
	      "params_int_text_null");

	/* 18c: empty params array */
	CHECK(parse("{\"q\":\"SELECT 1\",\"params\":[]}", &m) == 0 &&
	      m.n_params == 0, "params_empty");

	/* 18d: params must be an array */
	CHECK(parse("{\"q\":\"x\",\"params\":5}", &m) < 0, "params_not_array");

	/* 19: trailing garbage after object is ignored (we only require
	 * a valid object up front -- trailing content tolerated) */
	{
		int rc = parse("{\"q\":\"X\"} extra", &m);
		CHECK(rc == 0 && m.kind == QUACK_MSG_QUERY,
		      "trailing_garbage_ok");
	}

	/* 20: very long query */
	{
		size_t N = 4096;
		char *buf = (char *)malloc(N + 32);
		size_t i;
		strcpy(buf, "{\"q\":\"");
		for (i = 0; i < N; i++) buf[6 + i] = 'a';
		strcpy(buf + 6 + N, "\"}");
		CHECK(parse(buf, &m) == 0 && m.kind == QUACK_MSG_QUERY &&
		      m.q_len == N,
		      "long_query");
		free(buf);
	}

	/* 21: encoder hello */
	{
		quack_buf_t b;
		quack_buf_init(&b, 64);
		quack_emit_hello(&b, "0.1");
		CHECK(b.len > 0 &&
		      memcmp(b.p, "{\"hello\":\"sqlxtc\"", 17) == 0 &&
		      b.p[b.len - 1] == '\n',
		      "encode_hello");
		quack_buf_free(&b);
	}

	/* 22: encoder pong / done / err */
	{
		quack_buf_t b;
		quack_buf_init(&b, 64);
		quack_emit_pong(&b);
		quack_emit_done(&b, 7);
		quack_emit_err(&b, "boom");
		CHECK(strstr(b.p, "{\"pong\":1}\n") != NULL &&
		      strstr(b.p, "{\"done\":7}\n") != NULL &&
		      strstr(b.p, "{\"err\":\"boom\"}\n") != NULL,
		      "encode_pong_done_err");
		quack_buf_free(&b);
	}

	/* 23: encoder cols + rows */
	{
		quack_buf_t b;
		quack_buf_init(&b, 64);
		quack_emit_cols_begin(&b);
		quack_emit_cols_name(&b, 0, "id");
		quack_emit_cols_name(&b, 1, "name");
		quack_emit_cols_end(&b);
		quack_emit_row_begin(&b);
		quack_emit_row_int(&b, 0, 1);
		quack_emit_row_text(&b, 1, "alice", 5);
		quack_emit_row_end(&b);
		quack_emit_done(&b, 1);
		CHECK(strstr(b.p, "{\"cols\":[\"id\",\"name\"]}\n") != NULL &&
		      strstr(b.p, "{\"row\":[1,\"alice\"]}\n") != NULL,
		      "encode_cols_rows");
		quack_buf_free(&b);
	}

	/* 24: blob base64 */
	{
		quack_buf_t b;
		quack_buf_init(&b, 64);
		quack_emit_row_begin(&b);
		quack_emit_row_blob(&b, 0, "abc", 3);
		quack_emit_row_end(&b);
		CHECK(strstr(b.p, "\"YWJj\"") != NULL, "encode_blob_b64");
		quack_buf_free(&b);
	}

	/* 25: text escape control char */
	{
		quack_buf_t b;
		quack_buf_init(&b, 64);
		quack_append_jstring(&b, "a\nb", 3);
		CHECK(b.len == 6 && memcmp(b.p, "\"a\\nb\"", 6) == 0,
		      "encode_jstring_escape");
		quack_buf_free(&b);
	}

	printf("\nQuack tests: %d passed, %d failed\n", n_pass, n_fail);
	return n_fail ? 1 : 0;
}
