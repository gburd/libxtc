/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_io_reg.c
 *	The sim backend's fd REGISTRY (xtc_io_reg_fd / _mod_fd / _del_fd).
 *
 *	Every xtc_io backend must implement the same registry contract, and
 *	the other eight are covered by the m2 io tests -- but those run on a
 *	real backend, so the SIM backend's implementation
 *	(src/io/io_sim.c) was never exercised: no DST test registers an fd
 *	(they drive timers, aio and messaging).  That left the sim's
 *	registry, its grow path, and all of its error branches unrun, in the
 *	one backend the project's flagship test tier depends on.
 *
 *	This is a direct unit test of that contract rather than a scenario:
 *	  - register enough fds to force the regs[] array to GROW past its
 *	    initial capacity (16),
 *	  - reject a duplicate registration,
 *	  - modify an existing registration, and reject modifying one that
 *	    does not exist,
 *	  - delete registrations (including the not-registered case), and
 *	  - reject the argument errors every backend must reject.
 *
 *	It runs under the sim backend because that is the only build where
 *	these definitions are compiled (exactly one io_<backend>.c per
 *	build).
 */

#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_sim.h"

static int g_fail;

static void
check(int cond, const char *what)
{
	if (cond)
		return;
	printf("  FAIL: %s\n", what);
	g_fail++;
}

int
main(void)
{
	xtc_io_t *io = NULL;
	int i, tag_store[40];
	const int N = 40;   /* > the initial cap (16), so the array grows */

	xtc_sim_activate(0x10E9EA11u);

	if (xtc_io_init(&io) != XTC_OK) {
		printf("FAIL: xtc_io_init\n");
		return 1;
	}

	/* ---- argument validation (the contract every backend shares) ---- */
	check(xtc_io_reg_fd(NULL, 3, XTC_IO_READABLE, NULL) == XTC_E_INVAL,
	    "reg_fd(NULL io) rejected");
	check(xtc_io_reg_fd(io, -1, XTC_IO_READABLE, NULL) == XTC_E_INVAL,
	    "reg_fd(negative fd) rejected");
	/* An unknown fd is NOT_FOUND, not INVAL: the fd itself is legal, it
	 * simply is not registered here.  (A negative fd is likewise merely
	 * "not registered" to del/mod -- only reg_fd validates the fd, since
	 * it is the one that would store it.) */
	check(xtc_io_del_fd(io, -1) == XTC_E_NOTFOUND,
	    "del_fd(negative fd) -> NOTFOUND");
	check(xtc_io_del_fd(io, 12345) == XTC_E_NOTFOUND,
	    "del_fd(never registered) -> NOTFOUND");
	check(xtc_io_mod_fd(io, 12345, XTC_IO_READABLE, NULL) == XTC_E_NOTFOUND,
	    "mod_fd(never registered) -> NOTFOUND");
	check(xtc_io_mod_fd(NULL, 3, XTC_IO_READABLE, NULL) == XTC_E_INVAL,
	    "mod_fd(NULL io) rejected");
	check(xtc_io_del_fd(NULL, 3) == XTC_E_INVAL,
	    "del_fd(NULL io) rejected");

	/* ---- register N fds: forces at least one regs[] grow ---- */
	for (i = 0; i < N; i++) {
		tag_store[i] = i;
		if (xtc_io_reg_fd(io, 100 + i, XTC_IO_READABLE,
		    &tag_store[i]) != XTC_OK) {
			printf("  FAIL: reg_fd(%d) failed\n", 100 + i);
			g_fail++;
			break;
		}
	}

	/* A duplicate must be refused, not silently shadow the first. */
	check(xtc_io_reg_fd(io, 100, XTC_IO_READABLE, &tag_store[0])
	    == XTC_E_INVAL, "duplicate reg_fd rejected");

	/* ---- modify: interest and tag are both updatable ---- */
	check(xtc_io_mod_fd(io, 100, XTC_IO_WRITABLE, &tag_store[1])
	    == XTC_OK, "mod_fd on a live registration");
	check(xtc_io_mod_fd(io, 100 + (N - 1),
	    XTC_IO_READABLE | XTC_IO_WRITABLE, NULL) == XTC_OK,
	    "mod_fd to both interests, NULL tag");

	/* ---- delete: every registration, including the grown region ---- */
	for (i = 0; i < N; i++)
		check(xtc_io_del_fd(io, 100 + i) == XTC_OK,
		    "del_fd on a live registration");

	/* After deleting everything, the same fds must be registerable
	 * again (the slots were genuinely released, not just marked). */
	check(xtc_io_reg_fd(io, 100, XTC_IO_READABLE, &tag_store[0])
	    == XTC_OK, "re-register after delete");
	check(xtc_io_del_fd(io, 100) == XTC_OK, "delete the re-registered fd");

	(void)xtc_io_fini(io);

	if (g_fail != 0) {
		printf("FAIL: %d sim io-registry check(s) failed\n", g_fail);
		return 1;
	}
	printf("OK: sim io registry -- register/grow past the initial "
	    "capacity, duplicate + unknown-fd rejection, modify, delete, and "
	    "re-register after delete all behave per the xtc_io contract\n");
	return 0;
}
