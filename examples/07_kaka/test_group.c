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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

	/* Durability: commit under a temp dir, let the coordinator exit,
	 * spawn a fresh one over the same dir, and assert the offsets
	 * replayed. */
	{
		char dir[] = "/tmp/kaka-group-XXXXXX";
		int prc;
		if (mkdtemp(dir) == NULL) {
			perror("mkdtemp");
			return 2;
		}
		prc = group_persist_selftest(dir);
		if (prc == 0) {
			printf("  ok   group_persist_selftest "
			       "(offsets survive coordinator restart via plog)\n");
		} else {
			fprintf(stderr, "FAIL: group_persist_selftest step %d\n",
			    prc);
			rc = rc ? rc : prc;
		}
		/* Best-effort cleanup of the temp segment files. */
		{
			char cmd[256];
			int crc2;
			snprintf(cmd, sizeof cmd,
			    "find '%s' -type f -delete 2>/dev/null; "
			    "rmdir '%s' 2>/dev/null", dir, dir);
			crc2 = system(cmd);
			(void)crc2;
		}
	}

	printf("%s\n", rc == 0 ? "All kaka group tests passed."
	                       : "kaka group test FAILED.");
	return rc == 0 ? 0 : 1;
}
