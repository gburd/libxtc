/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/05_rexis/t_assert.h
 *	Test-only check macro shared by the rexis unit tests.  A failed
 *	ASSERT prints file:line and aborts the test process with a
 *	non-zero status (one test binary per file, pass/fail by exit
 *	code).
 */
#ifndef REXIS_T_ASSERT_H
#define REXIS_T_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define ASSERT(cond) do {                                                 \
	if (!(cond)) {                                                   \
		fprintf(stderr, "FAIL: %s:%d: %s\n",                     \
		    __FILE__, __LINE__, #cond);                          \
		exit(1);                                                 \
	}                                                                \
} while (0)

#endif /* REXIS_T_ASSERT_H */
