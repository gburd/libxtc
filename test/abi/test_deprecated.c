/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
/*
 * test/abi/test_deprecated.c -- XTC_DEPRECATED compiles and warns.
 *
 * Built twice by test/dist/test_deprecated.sh: once plain (must compile
 * and run; a use of a deprecated function must produce a "deprecated"
 * warning naming the message), and once with -DXTC_NO_DEPRECATION_WARNINGS
 * (must compile with no such warning).
 */
#include <stdio.h>

#include "xtc_export.h"

XTC_DEPRECATED("use xtc_abi_new_api") static int xtc_abi_old_api(int);

static int
xtc_abi_old_api(int x)
{
	return x + 1;
}

int
main(void)
{
	if (xtc_abi_old_api(1) != 2)
		return 1;
	printf("  [deprecated] ran\n");
	return 0;
}
