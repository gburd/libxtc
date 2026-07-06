/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_dst_inject.h
 *	DST bug-injection harness (the "bug-detection latency" yardstick).
 *
 *	FoundationDB / TigerBeetle back the claim "deterministic simulation
 *	finds real bugs" by PLANTING a known bug and proving the simulator
 *	catches it within a small number of seeds, deterministically, with
 *	a replayable trace.  That -- not a code-coverage percentage -- is
 *	the metric that inspires confidence: "if you break a safety
 *	invariant, DST catches it fast and hands you the exact seed."
 *
 *	This header defines the injection points.  In a normal build
 *	XTC_DST_INJECT_BUG is undefined and every check compiles to 0, so
 *	the harness is completely absent from production and from the
 *	default test build.  A dedicated build defines
 *	-DXTC_DST_INJECT_BUG=<n> to activate exactly ONE planted bug, and
 *	the DST suite is then expected to FAIL (an invariant fires) within
 *	a bounded seed count -- driven by scripts/dst-bug-inject.sh, which
 *	asserts each planted bug is caught.  A planted bug the sweep does
 *	NOT catch is a hole in the DST coverage of that safety property.
 *
 *	Each bug id targets a DST-reachable safety-critical site whose
 *	violation a specific invariant checker or capstone test detects:
 *
 *	  1  LOSTWAKE   -- proc.c drops a mailbox waker fire.  A parked
 *	                   receiver never wakes; the sim's quiescence /
 *	                   lost-wakeup invariant (a proc still alive but no
 *	                   runnable work and no timer) fires -> XTC_E_DEADLK.
 *	  2  LOCKEXCL   -- lock_mgr.c grants a conflicting lock.  Mutual
 *	                   exclusion breaks; test_sim_compose's lock-held
 *	                   witness (count must stay <= 1) fires.
 *	  3  NODURABLE  -- wal.c skips the fdatasync but still acks the
 *	                   commit.  An acked commit is not durable; the
 *	                   test_sim_compose_crash durability invariant (every
 *	                   acked commit present after recovery) fires.
 *
 *	When you add a new safety invariant, add a planted-bug id here and a
 *	case to scripts/dst-bug-inject.sh so DST must prove it catches it.
 */

#ifndef XTC_DST_INJECT_H
#define XTC_DST_INJECT_H

/*
 * XTC_DST_BUG(n) is 1 iff the build activated planted bug n.  Zero in
 * every normal build (XTC_DST_INJECT_BUG undefined), so all injection
 * sites vanish.  Exactly one bug is active per injected build.
 */
#if defined(XTC_DST_INJECT_BUG)
# define XTC_DST_BUG(n)  ((XTC_DST_INJECT_BUG) == (n))
#else
# define XTC_DST_BUG(n)  (0)
#endif

/* Symbolic ids (keep in sync with scripts/dst-bug-inject.sh). */
#define XTC_DST_BUG_LOSTWAKE   1   /* proc.c: drop a mailbox waker fire */
#define XTC_DST_BUG_LOCKEXCL   2   /* lock_mgr.c: grant a conflicting lock */
#define XTC_DST_BUG_NODURABLE  3   /* wal.c: skip fdatasync, still ack */

#endif /* XTC_DST_INJECT_H */
