/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_broker.c -- runs broker_selftest(), the in-process
 *	PRODUCE/FETCH round-trip through a real xtc loop (no socket,
 *	no daemon).  Exercises the partition proc, the conn->partition
 *	message structs, and the request-reply mailbox pattern.
 */

#include <stdio.h>
#include "broker.h"

int
main(void)
{
	int rc = broker_selftest();
	if (rc == 0)
		printf("  ok   broker_selftest (produce 3, fetch 3, shutdown)\n");
	else
		fprintf(stderr, "FAIL: broker_selftest step %d\n", rc);
	printf("%s\n", rc == 0 ? "All kaka broker tests passed."
	                       : "kaka broker test FAILED.");
	return rc == 0 ? 0 : 1;
}
