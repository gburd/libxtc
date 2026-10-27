/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_resp.c -- libFuzzer harness for the rexis RESP parser
 * (examples/05_rexis/proto.c), driven the way the connection drives it:
 * resp_parse_command in a loop over one receive buffer until it needs
 * more bytes or fails (conn.c), plus resp_parse over raw values.
 *
 *	Byte 0 picks the limits (max_bulk and max_inline small or default,
 *	as conn.c lowers max_bulk to the request cap); the rest is the
 *	stream, in an exact-size heap buffer.  Invariants: every argv entry
 *	points inside the buffer, a consumed count never runs past the end,
 *	on any result but RESP_OK the position is left at the command's
 *	start (the contract the connection's retry depends on), and
 *	argc <= max_args.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

#define FZ_MAX_ARGS 128 /* conn.c's MAX_ARGS */

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	resp_value_t argv[FZ_MAX_ARGS], v;
	resp_parser_t p;
	resp_err_t rc;
	size_t consumed, start, guard;
	char *buf;
	int argc, i, mode, max_args;

	if (n < 1 || n > 65536)
		return 0;
	mode = d[0];
	d++;
	n--;
	buf = malloc(n ? n : 1);
	fz_check(buf != NULL);
	memcpy(buf, d, n);

	max_args = (mode & 4) ? 4 : FZ_MAX_ARGS;
	resp_parser_init(&p, buf, n);
	if (mode & 1)
		p.max_bulk = 64;
	if (mode & 2)
		p.max_inline = 32;
	for (guard = 0; guard <= n; guard++) {
		start = p.pos;
		argc = -1;
		consumed = 0;
		rc = resp_parse_command(&p, argv, max_args, &argc, &consumed);
		if (rc != RESP_OK) {
			fz_check(p.pos == start);
			break;
		}
		fz_check(argc >= 0 && argc <= max_args);
		fz_check(consumed > 0 && consumed <= n - start);
		fz_check(p.pos == start + consumed);
		for (i = 0; i < argc; i++) {
			fz_check(argv[i].type == RESP_TYPE_BULK ||
			    argv[i].type == RESP_TYPE_SIMPLE);
			fz_check(argv[i].u.str.data >= buf &&
			    argv[i].u.str.data + argv[i].u.str.len <= buf + n);
		}
	}
	fz_check(guard <= n);

	/* Raw values (RESP3 types included). */
	resp_parser_init(&p, buf, n);
	for (guard = 0; guard <= n; guard++) {
		start = p.pos;
		if (resp_parse(&p, &v, &consumed) != RESP_OK)
			break;
		fz_check(
		    consumed > 0 && p.pos == start + consumed && p.pos <= n);
		if (v.type == RESP_TYPE_SIMPLE || v.type == RESP_TYPE_ERROR ||
		    v.type == RESP_TYPE_BULK)
			fz_check(v.u.str.data >= buf &&
			    v.u.str.data + v.u.str.len <= buf + n);
	}
	free(buf);
	return 0;
}
