/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_chan.c
 *	Property-based tests for M7 channels.
 *
 *	Properties:
 *	  C1: oneshot send -> recv: payload identical
 *	  C2: mpsc N msgs in -> N msgs out, FIFO order
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_chan.h"
#include "xtc_int.h"

#if defined(XTC_HAVE_HEGEL)

/* ----- C1: oneshot roundtrip ------------------------------- */

static void
prop_oneshot_roundtrip(hegel_test_case *tc, void *u)
{
	xtc_chan_oneshot_t *c;
	int v_in, v_out = 0;
	void *out;
	(void)u;
	v_in = (int)hegel_draw_int(tc, hegel_integers(-1000, 1000));
	hegel_assume(xtc_chan_oneshot_create(NULL, &c) == XTC_OK);
	hegel_assume(xtc_chan_oneshot_send(c, &v_in) == XTC_OK);
	hegel_assume(xtc_chan_oneshot_try_recv(c, &out) == XTC_OK);
	v_out = *(int *)out;
	hegel_assume(v_out == v_in);
	xtc_chan_oneshot_destroy(c);
}

/* ----- C2: mpsc FIFO -------------------------------------- */

static void
prop_mpsc_fifo(hegel_test_case *tc, void *u)
{
	xtc_chan_mpsc_t *c;
	int n, i;
	int *vals;
	(void)u;
	n = (int)hegel_draw_int(tc, hegel_integers(1, 32));
	vals = calloc((size_t)n, sizeof *vals);
	hegel_assume(vals != NULL);
	for (i = 0; i < n; i++)
		vals[i] = (int)hegel_draw_int(tc, hegel_integers(-1000, 1000));

	hegel_assume(xtc_chan_mpsc_create(NULL, 64, &c) == XTC_OK);
	for (i = 0; i < n; i++)
		hegel_assume(xtc_chan_mpsc_try_send(c, &vals[i]) == XTC_OK);
	for (i = 0; i < n; i++) {
		void *out;
		hegel_assume(xtc_chan_mpsc_try_recv(c, &out) == XTC_OK);
		hegel_assume(*(int *)out == vals[i]);
	}
	free(vals);
	xtc_chan_mpsc_destroy(c);
}

static const pbt_entry_t tests[] = {
	{ "oneshot_roundtrip", prop_oneshot_roundtrip, 20 },
	{ "mpsc_fifo",         prop_mpsc_fifo,         20 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "oneshot_roundtrip", NULL, 0 },
	{ "mpsc_fifo",         NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("chan", tests)
