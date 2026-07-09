/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/05_sharp_edges.c -- the POSIX/libc sharp-edges
 * trio: thread-safe environment access, a per-thread seedable PRNG, and
 * a bounded string copy that always NUL-terminates.  Uses only the
 * public xtc_* API.
 */

/* !region full */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "xtc.h"

int
main(void)
{
	char path[16];
	uint64_t a, b;

	/* 1. Thread-safe env: set then read back.  Guarded internally so a
	 *    concurrent xtc_env_set from another thread cannot invalidate
	 *    the pointer xtc_env_get returns. */
	if (xtc_env_set("XTC_DEMO", "on", 1) != XTC_OK)
		return 1;
	printf("XTC_DEMO=%s\n", xtc_env_get("XTC_DEMO"));

	/* 2. Seedable per-thread PRNG: the same seed replays the same
	 *    stream (each thread has its own state). */
	xtc_rand_seed(42);
	a = xtc_rand_u64();
	xtc_rand_seed(42);
	b = xtc_rand_u64();
	if (a != b) {
		fprintf(stderr, "seed not reproducible\n");
		return 1;
	}
	printf("rand(42) reproducible: %llu\n", (unsigned long long)a);

	/* 3. Bounded copy: always NUL-terminates; the return is the length
	 *    it TRIED to create, so >= dstsize means truncated. */
	if (xtc_strlcpy(path, "verylongvalue", sizeof path) >= sizeof path)
		printf("truncated to \"%s\"\n", path);

	return 0;
}
/* !endregion full */
