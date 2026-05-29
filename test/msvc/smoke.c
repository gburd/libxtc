/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/msvc/smoke.c
 *
 *	A dependency-free smoke test for the MSVC build.  The munit
 *	harness uses GCC-isms (VLA array parameters via
 *	MUNIT_ARRAY_PARAM, GCC pragmas) that MSVC rejects, so this
 *	standalone test links xtc.lib and exercises a representative
 *	slice of the library that does not need the harness:
 *
 *	  - version + strerror (pure functions)
 *	  - the monotonic / realtime clocks (the Win32 os_time path)
 *	  - a slab cache alloc/free round-trip (Win32 TLS magazines)
 *	  - an lwlock acquire/release (the lock fast path)
 *
 *	Exit code 0 = pass, nonzero = the first failed check.
 */

#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_slab.h"
#include "xtc_lwlock.h"
#include "os_time.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

int
main(void)
{
	/* --- version --- */
	{
		const char *v = xtc_version_string();
		CHECK(v != NULL && v[0] != '\0');
		printf("  ok   version = %s\n", v);
	}

	/* --- strerror --- */
	{
		const char *e = xtc_strerror(XTC_E_NOMEM);
		CHECK(e != NULL && e[0] != '\0');
		printf("  ok   strerror(XTC_E_NOMEM) = %s\n", e);
	}

	/* --- clocks (Win32 os_time path) --- */
	{
		int64_t a = 0, b = 0, r = 0;
		CHECK(__os_clock_mono(&a) == XTC_OK);
		CHECK(__os_clock_mono(&b) == XTC_OK);
		CHECK(b >= a);                 /* monotonic non-decreasing */
		CHECK(__os_clock_real(&r) == XTC_OK);
		CHECK(r > 0);
		printf("  ok   clocks: mono delta=%lld ns, real>0\n",
		    (long long)(b - a));
	}

	/* --- slab cache (Win32 TLS magazines) --- */
	{
		xtc_slab_t *s = NULL;
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		void *p, *q;
		o.obj_size = 64;
		CHECK(xtc_slab_create(&o, &s) == XTC_OK);
		p = xtc_slab_alloc(s);
		CHECK(p != NULL);
		memset(p, 0xab, 64);
		xtc_slab_free(s, p);
		q = xtc_slab_alloc(s);          /* should recycle */
		CHECK(q != NULL);
		xtc_slab_free(s, q);
		xtc_slab_destroy(s);
		printf("  ok   slab alloc/free round-trip\n");
	}

	/* --- lwlock fast path --- */
	{
		xtc_lwlock_t lw;
		CHECK(xtc_lwlock_init(&lw, 0) == XTC_OK);
		CHECK(xtc_lwlock_acquire(&lw, XTC_LW_EXCLUSIVE) == XTC_OK);
		xtc_lwlock_release(&lw);
		CHECK(xtc_lwlock_acquire(&lw, XTC_LW_SHARED) == XTC_OK);
		xtc_lwlock_release(&lw);
		xtc_lwlock_destroy(&lw);
		printf("  ok   lwlock acquire/release (X + S)\n");
	}

	printf("All MSVC smoke checks passed.\n");
	return 0;
}
