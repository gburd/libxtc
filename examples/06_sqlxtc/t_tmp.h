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

#endif /* SQLXTC_T_TMP_H */
