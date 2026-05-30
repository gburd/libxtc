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
#include <stdint.h>
#include "broker.h"

int
main(void)
{
	int rc = broker_selftest();
	if (rc == 0)
		printf("  ok   broker_selftest (produce 3, fetch 3, shutdown)\n");
	else
		fprintf(stderr, "FAIL: broker_selftest step %d\n", rc);

	/* Credit-based backpressure: a producer with a 4-credit budget
	 * streams 200 records to the partition.  Assert it never had
	 * more than 4 requests in flight and every record landed. */
	{
		int maxif = -1;
		uint64_t hwm = 0;
		int crc = broker_credit_selftest(&maxif, &hwm);
		if (crc == 0 && maxif >= 1 && maxif <= 4 && hwm == 200) {
			printf("  ok   broker_credit_selftest "
			       "(200 records, peak in-flight=%d<=4, hwm=%llu)\n",
			       maxif, (unsigned long long)hwm);
		} else {
			fprintf(stderr, "FAIL: credit selftest rc=%d "
			        "maxif=%d hwm=%llu\n",
			        crc, maxif, (unsigned long long)hwm);
			rc = rc ? rc : 100;
		}
	}

	printf("%s\n", rc == 0 ? "All kaka broker tests passed."
	                       : "kaka broker test FAILED.");
	return rc == 0 ? 0 : 1;
}
