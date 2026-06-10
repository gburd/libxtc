/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/07_kaka/t_assert.h
 *	Test-only check macro shared by the kaka unit tests.  A failed
 *	ASSERT prints file:line and aborts the test process with a
 *	non-zero status (these tests are pass/fail-by-exit-code, run one
 *	per binary).
 */
#ifndef KAKA_T_ASSERT_H
#define KAKA_T_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define ASSERT(c) do {                                                    \
	if (!(c)) {                                                       \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(1);                                                 \
	}                                                                \
} while (0)

#endif /* KAKA_T_ASSERT_H */
