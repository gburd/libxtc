/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/11_lorb/t_assert.h
 *	Test-only check macro shared by the lorb unit tests.  A failed
 *	ASSERT prints file:line and aborts the test process with a
 *	non-zero status.
 */
#ifndef LORB_T_ASSERT_H
#define LORB_T_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define ASSERT(c) do {                                                    \
	if (!(c)) {                                                       \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(1);                                                 \
	}                                                                \
} while (0)

#endif /* LORB_T_ASSERT_H */
