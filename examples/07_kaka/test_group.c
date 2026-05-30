/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_group.c -- runs group_selftest(), the in-process consumer-group
 *	offset coordinator round-trip through a real xtc loop (no socket,
 *	no daemon).  Exercises the single-owner coordinator proc and the
 *	commit / fetch message protocol.
 */

#include <stdio.h>
#include "group.h"

int
main(void)
{
	int rc = group_selftest();
	if (rc == 0)
		printf("  ok   group_selftest "
		       "(commit/fetch, last-write-wins, group+partition isolation)\n");
	else
		fprintf(stderr, "FAIL: group_selftest step %d\n", rc);
	printf("%s\n", rc == 0 ? "All kaka group tests passed."
	                       : "kaka group test FAILED.");
	return rc == 0 ? 0 : 1;
}
