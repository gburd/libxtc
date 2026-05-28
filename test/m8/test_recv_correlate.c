/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m8/test_recv_correlate.c -- xtc_recv_correlate verification.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_int.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* fork-join: parent spawns N children, sends each a request with a
 * shared correlation prefix, waits for N replies via xtc_recv_correlate. */

#define N_KIDS 5

struct kid_args {
	xtc_pid_t parent;
	uint32_t  corr;
	int       child_id;
};

struct reply {
	uint32_t corr;        /* matches parent's correlation prefix */
	int      result;      /* this child's contribution */
};

static void
kid_proc(void *arg)
{
	struct kid_args *a = arg;
	struct reply r;
	r.corr = a->corr;
	r.result = a->child_id * 10;
	(void)xtc_send(a->parent, &r, sizeof r);
}

static _Atomic int g_collected;
static _Atomic int g_total_result;

static void
parent_proc(void *arg)
{
	xtc_loop_t *loop = (xtc_loop_t *)arg;
	struct kid_args ka[N_KIDS];
	xtc_pid_t kid_pids[N_KIDS];
	xtc_msg_t replies[N_KIDS];
	uint32_t  corr = 0xCAFEBABE;
	int       n_recv = 0;
	int       i, rc, total = 0;

	for (i = 0; i < N_KIDS; i++) {
		ka[i].parent = xtc_self();
		ka[i].corr = corr;
		ka[i].child_id = i;
		(void)xtc_proc_spawn(loop, kid_proc, &ka[i], NULL,
		    &kid_pids[i]);
	}

	rc = xtc_recv_correlate(&corr, sizeof corr, N_KIDS, replies,
	    &n_recv, 5LL * 1000 * 1000 * 1000);

	atomic_store(&g_collected, n_recv);
	for (i = 0; i < n_recv; i++) {
		struct reply *r = replies[i].data;
		total += r->result;
		__os_free(replies[i].data);
	}
	atomic_store(&g_total_result, total);
	(void)rc;

	xtc_loop_stop(loop);
}

static MunitResult
test_correlate_fork_join(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_collected, 0);
	atomic_store(&g_total_result, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, parent_proc, loop, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_collected), ==, N_KIDS);
	/* sum of 0..4 * 10 = 100 */
	munit_assert_int(atomic_load(&g_total_result), ==, 100);

	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* Bad-args test. */
static MunitResult
test_correlate_bad_args(const MunitParameter p[], void *d)
{
	xtc_msg_t msgs[2];
	int n;
	uint32_t c = 1;
	(void)p; (void)d;
	munit_assert_int(xtc_recv_correlate(NULL, 4, 1, msgs, &n, 0),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_recv_correlate(&c, 0, 1, msgs, &n, 0),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_recv_correlate(&c, 4, 0, msgs, &n, 0),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_recv_correlate(&c, 4, 1, NULL, &n, 0),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_recv_correlate(&c, 4, 1, msgs, NULL, 0),
	    ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/correlate_fork_join", test_correlate_fork_join, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/correlate_bad_args",  test_correlate_bad_args,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m8/recv_correlate", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
