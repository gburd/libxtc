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
	int rc = quack_parse(line, strlen(line), &m);
	int ok = want_ok ? rc == 0 : rc != 0;
	printf("  %s %s (rc=%d)\n", ok ? "OK  " : "FAIL", name, rc);
	if (!ok)
		n_fail++;
}

int
main(void)
{
	quack_msg_t m;
	const char *max = "{\"q\":\"x\",\"limit\":9223372036854775807}";
	const char *min = "{\"q\":\"x\",\"params\":[-9223372036854775808]}";

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

	printf("quack range: %s\n", n_fail ? "FAIL" : "OK");
	return n_fail ? 1 : 0;
}
