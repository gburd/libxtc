/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/tnt/test_tnt_timer_stop.c
 *	Stopping the runtime with a timer still armed frees the timer.
 *
 *	xtc_tnt_register_timer allocates an argument block for the fiber
 *	that sleeps and then posts the timer; the fiber freed it when it
 *	fired.  A runtime stopped first frees the fiber with its loop, so the
 *	block leaked: 32 bytes per pending timer (a venue whose books each
 *	re-arm a 100 ms tick leaked one per book at every stop, which is
 *	what LeakSanitizer reported for examples/16_l2book).  The shard now
 *	keeps its armed timers on a list and shard_destroy frees what is
 *	left.
 *
 *	Oracle: under LeakSanitizer (the sanitizer CI builds) the run exits
 *	clean; without one it checks only that a stop with timers pending
 *	returns.  The leak itself is LSan's to see.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_tnt.h"

#if defined(_WIN32)
int
main(void)
{
	printf("SKIP: xtc_tnt is POSIX-only\n");
	return 0;
}
#else

#define NARM 8

static int g_armed;

typedef struct {
	int unused;
} arm_t;

static xtc_tnt_transition_t
arm_init(void *self, const void *a, size_t n)
{
	int i;

	(void)self;
	(void)a;
	(void)n;
	/* Far in the future: these never fire before the stop. */
	for (i = 0; i < NARM; i++)
		xtc_tnt_register_timer(60LL * 1000 * 1000 * 1000, 1);
	xtc_atomic_fetch_add_i32(&g_armed, NARM, xtc_mo_seq_cst);
	xtc_tnt_stop();
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
arm_handler(void *self, xtc_tnt_message_t *msg)
{
	(void)self;
	(void)msg;
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static const xtc_tnt_type_t types[] = {
	{ 0, "arm", 1, sizeof(arm_t), 0, 4, 1, arm_init, arm_handler },
};

int
main(void)
{
	xtc_tnt_spec_t spec;
	int rc;

	memset(&spec, 0, sizeof spec);
	spec.name = "timerstop";
	spec.types = types;
	spec.n_types = 1;
	spec.shard_count = 1;
	spec.boot_type = 0;
	rc = xtc_tnt_start(&spec);
	if (rc != 0 || xtc_atomic_load_i32(&g_armed, xtc_mo_seq_cst) != NARM) {
		printf("FAIL: rc=%d armed=%d\n", rc,
		    xtc_atomic_load_i32(&g_armed, xtc_mo_seq_cst));
		return 1;
	}
	printf("PASS: %d timers armed at the stop, all freed\n", NARM);
	return 0;
}
#endif
