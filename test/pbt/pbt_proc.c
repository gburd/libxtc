/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_proc.c
 *	Property-based tests for the M8 proc/mailbox layer.
 *
 *	Properties (kept simple and deterministic):
 *	  Mp1: spawn N processes, all complete (no loss).
 *	  Mp2: send K bytes, receive identical K bytes back.
 *	  Mp3: K-message FIFO order is preserved.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_int.h"

#if defined(XTC_HAVE_HEGEL)

/* ----- Mp1: spawn N processes, all complete ----------------- */

static _Atomic int mp1_done;

static void mp1_proc(void *arg)
{
	(void)arg;
	atomic_fetch_add(&mp1_done, 1);
}

static void
prop_spawn_n_all_complete(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	int n, i;
	(void)u;
	n = (int)hegel_draw_int(tc, hegel_integers(1, 32));
	atomic_store(&mp1_done, 0);
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	for (i = 0; i < n; i++) {
		xtc_pid_t p;
		hegel_assume(xtc_proc_spawn(loop, mp1_proc, NULL, NULL, &p)
		    == XTC_OK);
	}
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	hegel_assume(atomic_load(&mp1_done) == n);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

/* ----- Mp2: send K bytes, recv K bytes back unchanged ------- */

struct mp2_state { uint8_t buf[64]; size_t k; int ok; };

static void mp2_proc(void *arg)
{
	struct mp2_state *s = arg;
	void *m; size_t sz;
	if (xtc_recv(&m, &sz, 200LL * 1000 * 1000) != XTC_OK) return;
	if (sz != s->k) { __os_free(m); return; }
	if (memcmp(m, s->buf, s->k) != 0) { __os_free(m); return; }
	s->ok = 1;
	__os_free(m);
}

static void
prop_send_recv_roundtrip(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	struct mp2_state s = {0};
	size_t i;
	(void)u;
	s.k = (size_t)hegel_draw_int(tc, hegel_integers(1, 64));
	for (i = 0; i < s.k; i++)
		s.buf[i] = (uint8_t)hegel_draw_int(tc, hegel_integers(0, 255));
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	hegel_assume(xtc_proc_spawn(loop, mp2_proc, &s, NULL, &pid) == XTC_OK);
	hegel_assume(xtc_send(pid, s.buf, s.k) == XTC_OK);
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	hegel_assume(s.ok == 1);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

/* ----- Mp3: K-message FIFO ----------------------------------- */

struct mp3_state { int *exp; int n; int ok; };

static void mp3_proc(void *arg)
{
	struct mp3_state *s = arg;
	int i;
	for (i = 0; i < s->n; i++) {
		void *m; size_t sz;
		if (xtc_recv(&m, &sz, 200LL * 1000 * 1000) != XTC_OK) return;
		if (sz != sizeof(int) || *(int *)m != s->exp[i]) {
			__os_free(m);
			return;
		}
		__os_free(m);
	}
	s->ok = 1;
}

static void
prop_fifo_order(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	struct mp3_state s = {0};
	int *vals;
	int i;
	(void)u;
	s.n = (int)hegel_draw_int(tc, hegel_integers(1, 16));
	vals = calloc((size_t)s.n, sizeof *vals);
	hegel_assume(vals != NULL);
	for (i = 0; i < s.n; i++)
		vals[i] = (int)hegel_draw_int(tc, hegel_integers(-1000, 1000));
	s.exp = vals;
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	hegel_assume(xtc_proc_spawn(loop, mp3_proc, &s, NULL, &pid) == XTC_OK);
	for (i = 0; i < s.n; i++)
		hegel_assume(xtc_send(pid, &vals[i], sizeof(int)) == XTC_OK);
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	hegel_assume(s.ok == 1);
	free(vals);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

static const pbt_entry_t tests[] = {
	{ "spawn_n_all_complete",  prop_spawn_n_all_complete,  20 },
	{ "send_recv_roundtrip",   prop_send_recv_roundtrip,   20 },
	{ "fifo_order",            prop_fifo_order,            20 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "spawn_n_all_complete", NULL, 0 },
	{ "send_recv_roundtrip",  NULL, 0 },
	{ "fifo_order",           NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("proc", tests)
