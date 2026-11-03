/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/real/res_harness.c
 *
 *	REAL CODE: links src/ptc/res.c unchanged.  Supersedes the
 *	transcribed ../res_harness.c, whose release was the pre-1.59
 *	fetch_sub-then-clamp (res.c replaced it with a flooring CAS on
 *	2026-10-12, 8f85d0d2), so that harness had been proving code the
 *	library no longer ran.
 *
 *	PROPERTIES, two actors each acquiring then releasing n units of
 *	one kind against a cap, over every interleaving and every n,
 *	cap and starting `used`:
 *	  - no over-admission: `used` never exceeds the cap after a
 *	    granted acquire (the CAS loop's headroom test);
 *	  - no signed overflow anywhere in acquire/release, including a
 *	    cap of INT64_MAX (the 1.59 bug: cur + n wrapped negative);
 *	  - `used` never goes negative (release floors at zero in one CAS);
 *	  - conservation: once both actors are done, `used` is back where
 *	    it started (no unit leaked or double-freed).
 *	A cap of INT64_MAX starts `used` within 3 of it, so the headroom
 *	test is exercised at the edge where the 1.59 sum wrapped.
 *	Planted bugs (the SAME ids the DST tier plants, so one switch
 *	breaks both tiers), each must FAIL: -DXTC_DST_INJECT_BUG=6
 *	(RESOVER, skip the cap check) and =5 (RESLEAK, drop the release).
 */
#include "xtc.h"
#include "xtc_res.h"

int64_t __os_mem_max(void) { return 0; }	/* not on this path */

static xtc_res_t R;
static int64_t CAP;		/* set nondeterministically in main */
int64_t nondet_int64(void);
static _Atomic int64_t held;	/* units granted and not yet released */
static _Atomic int done;

static void
actor(void)
{
	int64_t n, u;

	__CPROVER_assume(n >= 1 && n <= 2);
	if (xtc_res_acquire(&R, XTC_RES_TASKS, n) == XTC_OK) {
		/* Units held at once by the start load plus the actors: the
		 * sum the cap bounds, independent of when `used` is read. */
		u = atomic_fetch_add(&held, n) + n;
		__CPROVER_assert(CAP <= 0 || u <= CAP,
		    "granted units never exceed the cap (no over-admission)");
		atomic_fetch_sub(&held, n);
		xtc_res_release(&R, XTC_RES_TASKS, n);
	}
	__CPROVER_assert(xtc_res_used(&R, XTC_RES_TASKS) >= 0,
	    "used never goes negative");
	atomic_fetch_add(&done, 1);
}

int
main(void)
{
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	int64_t start;

	CAP = nondet_int64();
	__CPROVER_assume(CAP == 0 || CAP == 1 || CAP == 2 ||
	    CAP == INT64_MAX);
	caps.tasks = CAP;
	if (xtc_res_init(&R, &caps) != XTC_OK)
		return 0;
	/* Start anywhere at or under the cap (a long-running process). */
	__CPROVER_assume(start >= 0 && (CAP <= 0 || start <= CAP));
	__CPROVER_assume(CAP != INT64_MAX || start >= INT64_MAX - 3);
	atomic_store(&R.used[XTC_RES_TASKS], start);
	atomic_store(&held, start);

	__CPROVER_ASYNC_1: actor();
	actor();
	__CPROVER_assume(atomic_load(&done) == 2);
	__CPROVER_assert(xtc_res_used(&R, XTC_RES_TASKS) == start,
	    "conservation: balanced acquire/release returns used to start");
	return 0;
}
