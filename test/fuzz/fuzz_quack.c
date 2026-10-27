/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_quack.c -- libFuzzer harness for sqlxtc's quack wire
 * protocol, decoded the way conn.c decodes a client's bytes: the stream
 * is cut into lines at '\n' (process_lines), each line is parsed in place
 * by quack_parse, and a QUERY's statement text then goes through the
 * classifier conn.c runs before executing it (sql_parse).
 *
 *	Each line is copied to an exact-size heap buffer with NO terminator
 *	(the connection's read buffer has none either), so a read past the
 *	line is an ASan report.  On success the message must be coherent:
 *	the kind is known, at most QUACK_MAX_PARAMS params, and every text
 *	or blob param points inside the line.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "quack.h"
#include "sql_parse.h"

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

static void
fz_line(const uint8_t *d, size_t n)
{
	quack_msg_t m;
	sql_info_t info;
	char *line;
	int i;

	line = malloc(n ? n : 1);
	fz_check(line != NULL);
	memcpy(line, d, n);
	memset(&m, 0, sizeof m);
	if (quack_parse(line, n, &m) == 0) {
		fz_check(
		    m.kind >= QUACK_MSG_NONE && m.kind <= QUACK_MSG_UNKNOWN);
		fz_check(m.n_params >= 0 && m.n_params <= QUACK_MAX_PARAMS);
		for (i = 0; i < m.n_params; i++) {
			if (m.params[i].type != QUACK_P_TEXT &&
			    m.params[i].type != QUACK_P_BLOB)
				continue;
			fz_check(m.params[i].sval >= line &&
			    m.params[i].sval + m.params[i].slen <= line + n);
		}
		if (m.kind == QUACK_MSG_QUERY) {
			fz_check(m.q != NULL);
			(void)sql_parse(m.q, m.q_len, &info);
		}
	} else {
		fz_check(m.err != NULL);
	}
	free(line);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	const uint8_t *nl;

	if (n > 65536)
		return 0;
	while (n > 0) {
		nl = memchr(d, '\n', n);
		if (nl == NULL) {
			fz_line(d, n); /* a final unterminated line */
			break;
		}
		fz_line(d, (size_t)(nl - d));
		n -= (size_t)(nl - d) + 1;
		d = nl + 1;
	}
	return 0;
}
