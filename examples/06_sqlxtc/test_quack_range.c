/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/06_sqlxtc/test_quack_range.c
 *	An out-of-range JSON integer must be REJECTED, not wrapped or
 *	clamped (PLAN 19.27.23).  "limit" and "ping" go through jp_int,
 *	params through jp_number/strtoll; both used to accept a 23-digit
 *	value (limit wrapped to an arbitrary positive number via signed
 *	overflow, a param saturated to LLONG_MAX).  Standalone; exit 1 on
 *	any failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "quack.h"

static int n_fail;

static void
expect(const char *line, int want_ok, const char *name)
{
	quack_msg_t m;
	char buf[256];
	int rc;
	snprintf(buf, sizeof buf, "%s", line);
	rc = quack_parse(buf, strlen(buf), &m);
	int ok = want_ok ? rc == 0 : rc != 0;
	printf("  %s %s (rc=%d)\n", ok ? "OK  " : "FAIL", name, rc);
	if (!ok)
		n_fail++;
}

int
main(void)
{
	quack_msg_t m;
	char max[] = "{\"q\":\"x\",\"limit\":9223372036854775807}";
	char min[] = "{\"q\":\"x\",\"params\":[-9223372036854775808]}";
	char esc[] = "{\"q\":\"x\",\"params\":[\"a\\\"b\\\\\\u0001\\u00e9\\u20ac\"]}";

	expect("{\"q\":\"x\",\"limit\":99999999999999999999999}", 0,
	    "limit_23_digits_rejected");
	expect("{\"q\":\"x\",\"limit\":9223372036854775808}", 0,
	    "limit_int64_max_plus_1_rejected");
	expect("{\"ping\":99999999999999999999999}", 0,
	    "ping_23_digits_rejected");
	expect("{\"q\":\"x\",\"params\":[99999999999999999999999]}", 0,
	    "param_23_digits_rejected");
	expect("{\"q\":\"x\",\"params\":[-99999999999999999999999]}", 0,
	    "param_neg_23_digits_rejected");

	/* The boundaries themselves still parse, to the exact value. */
	if (quack_parse(max, strlen(max), &m) != 0 || m.limit != INT64_MAX) {
		printf("  FAIL limit_int64_max_accepted\n");
		n_fail++;
	} else
		printf("  OK   limit_int64_max_accepted\n");
	if (quack_parse(min, strlen(min), &m) != 0 ||
	    m.params[0].ival != INT64_MIN) {
		printf("  FAIL param_int64_min_accepted\n");
		n_fail++;
	} else
		printf("  OK   param_int64_min_accepted\n");

	/* An escaped TEXT param decodes in place (\uXXXX as UTF-8). */
	if (quack_parse(esc, strlen(esc), &m) != 0 || m.n_params != 1 ||
	    m.params[0].slen != 10 ||
	    memcmp(m.params[0].sval, "a\"b\\\x01\xc3\xa9\xe2\x82\xac", 10) != 0) {
		printf("  FAIL param_escaped_text_decoded\n");
		n_fail++;
	} else
		printf("  OK   param_escaped_text_decoded\n");

	printf("quack range: %s\n", n_fail ? "FAIL" : "OK");
	return n_fail ? 1 : 0;
}
