/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_credit.c
 *	Sliding-window credit regulator: window accounting, try-acquire
 *	exhaustion, release-without-acquire rejection, peak tracking, and a
 *	real in-flight window over a request/reply RPC between two procs
 *	(the issuer never exceeds the window; every op is acknowledged).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_credit.h"
#include "xtc_int.h"

/* ---------- accounting, no procs ---------- */

static MunitResult
test_credit_accounting(const MunitParameter p[], void *d)
{
	xtc_credit_t *c = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_credit_create(0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_credit_create(3, &c), ==, XTC_OK);
	munit_assert_uint(xtc_credit_window(c), ==, 3);
	munit_assert_uint(xtc_credit_in_flight(c), ==, 0);

	/* Take all three. */
	munit_assert_int(xtc_credit_try_acquire(c), ==, XTC_OK);
	munit_assert_int(xtc_credit_try_acquire(c), ==, XTC_OK);
	munit_assert_int(xtc_credit_try_acquire(c), ==, XTC_OK);
	munit_assert_uint(xtc_credit_in_flight(c), ==, 3);
	munit_assert_uint(xtc_credit_peak(c), ==, 3);

	/* Window exhausted. */
	munit_assert_int(xtc_credit_try_acquire(c), ==, XTC_E_AGAIN);

	/* Return one, take one -- peak stays at 3. */
	munit_assert_int(xtc_credit_release(c), ==, XTC_OK);
	munit_assert_uint(xtc_credit_in_flight(c), ==, 2);
	munit_assert_int(xtc_credit_try_acquire(c), ==, XTC_OK);
	munit_assert_uint(xtc_credit_peak(c), ==, 3);

	/* Drain fully; a release past zero is a caller bug -> XTC_E_INVAL. */
	munit_assert_int(xtc_credit_release(c), ==, XTC_OK);
	munit_assert_int(xtc_credit_release(c), ==, XTC_OK);
	munit_assert_int(xtc_credit_release(c), ==, XTC_OK);
	munit_assert_uint(xtc_credit_in_flight(c), ==, 0);
	munit_assert_int(xtc_credit_release(c), ==, XTC_E_INVAL);

	xtc_credit_destroy(c);
	return MUNIT_OK;
}

/* ---------- real sliding window over request/reply ---------- */

#define WINDOW 4
#define TOTAL  100

struct worker_state { xtc_pid_t issuer; };

/* Worker: receives a request carrying the issuer's pid + a seq, replies
 * with the seq (the reply returns the credit to the issuer). */
static void
worker_proc(void *arg)
{
	(void)arg;
	for (;;) {
		void *msg = NULL; size_t n = 0;
		xtc_pid_t from;
		int seq;
		if (xtc_recv(&msg, &n, 2000LL * 1000000) != XTC_OK || msg == NULL)
			return;
		if (n != sizeof(xtc_pid_t) + sizeof(int)) { xtc_free(msg); return; }
		memcpy(&from, msg, sizeof from);
		memcpy(&seq, (char *)msg + sizeof from, sizeof seq);
		xtc_free(msg);
		if (seq < 0) return;   /* shutdown sentinel */
		(void)xtc_send(from, &seq, sizeof seq);
	}
}

struct issuer_state {
	xtc_pid_t worker;
	int       result;         /* 0 = ok */
	unsigned  peak;           /* observed peak in flight */
	int       acked;
};

/* Issuer: keeps at most WINDOW requests in flight using the credit
 * regulator.  Fill credits, then for each ack (reply) release a credit
 * and issue the next, until all TOTAL are acknowledged. */
static void
issuer_proc(void *arg)
{
	struct issuer_state *st = arg;
	xtc_credit_t *cw = NULL;
	xtc_pid_t self = xtc_self();
	int sent = 0;
	char req[sizeof(xtc_pid_t) + sizeof(int)];

	if (xtc_credit_create(WINDOW, &cw) != XTC_OK) { st->result = 1; return; }
	memcpy(req, &self, sizeof self);

	while (st->acked < TOTAL) {
		/* Fill the window with outstanding requests. */
		while (sent < TOTAL &&
		    xtc_credit_try_acquire(cw) == XTC_OK) {
			memcpy(req + sizeof self, &sent, sizeof sent);
			if (xtc_send(st->worker, req, sizeof req) != XTC_OK) {
				st->result = 2; goto done;
			}
			sent++;
		}
		/* The window must never be exceeded. */
		if (xtc_credit_in_flight(cw) > WINDOW) { st->result = 3; goto done; }

		/* Wait for an ack; releasing its credit frees a slot. */
		{
			void *msg = NULL; size_t n = 0;
			if (xtc_recv(&msg, &n, 2000LL * 1000000) != XTC_OK) {
				st->result = 4; goto done;
			}
			if (msg) xtc_free(msg);
			(void)xtc_credit_release(cw);
			st->acked++;
		}
	}
done:
	st->peak = xtc_credit_peak(cw);
	/* Shut the worker down. */
	{
		int stop = -1;
		memcpy(req + sizeof self, &stop, sizeof stop);
		(void)xtc_send(st->worker, req, sizeof req);
	}
	xtc_credit_destroy(cw);
}

static MunitResult
test_credit_sliding_window(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct issuer_state ist;
	(void)p; (void)d;

	memset(&ist, 0, sizeof ist);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, worker_proc, NULL, NULL,
	    &ist.worker), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, issuer_proc, &ist, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(ist.result, ==, 0);
	munit_assert_int(ist.acked, ==, TOTAL);      /* all acknowledged */
	munit_assert_uint(ist.peak, <=, WINDOW);     /* never exceeded window */
	munit_assert_uint(ist.peak, >, 1);           /* did overlap (pipelined) */

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/accounting",     test_credit_accounting,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sliding_window", test_credit_sliding_window, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.9/credit", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
