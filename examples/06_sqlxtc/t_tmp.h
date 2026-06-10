/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/t_tmp.h
 *	Test-only helper: build an mkstemp template under $TMPDIR (default
 *	/tmp), so test scratch files land where the environment directs
 *	instead of always polluting /tmp.  CI and default runs are
 *	unaffected (TMPDIR unset -> /tmp).
 */
#ifndef SQLXTC_T_TMP_H
#define SQLXTC_T_TMP_H

#include <stdio.h>
#include <stdlib.h>

static inline void
t_tmpl(char *out, size_t cap, const char *prefix)
{
	const char *d = getenv("TMPDIR");
	if (d == NULL || d[0] == '\0')
		d = "/tmp";
	snprintf(out, cap, "%s/%s-XXXXXX", d, prefix);
}

/* Shared check macro for the hand-rolled (non-munit) tests.  A failed
 * CK records file:line and sets g_fail; the test's main() returns
 * g_fail at the end, so every failure is reported, not just the first.
 * Each test translation unit gets its own g_fail.  Marked maybe-unused
 * so a test that includes this only for t_tmpl() (and never uses CK)
 * does not trip -Wunused-variable / -Werror.  Define T_NO_GFAIL before
 * including to suppress it entirely (for a unit that declares its own,
 * e.g. an atomic g_fail). */
#ifndef T_NO_GFAIL
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int g_fail;
#endif
#ifndef CK
#define CK(c) do {                                                        \
	if (!(c)) {                                                       \
		fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
		g_fail = 1;                                              \
	}                                                                \
} while (0)
#endif

#endif /* SQLXTC_T_TMP_H */
