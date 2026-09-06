/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_svr.c -- verifies M10.5 gen_server (xtc_svr).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_svr.h"
#include "xtc_reg.h"
#include "xtc_int.h"

/* A trivial counter-server: cast to increment, call to read. */
struct counter_state {
	int count;
};

static int
counter_handle_call(void *st, const void *req, size_t size,
                    xtc_svr_call_t *call)
{
	struct counter_state *s = st;
	int v = s->count;
	(void)req; (void)size;
	(void)xtc_svr_reply(call, &v, sizeof v);
	return XTC_SVR_CONTINUE;
}

static int
counter_handle_cast(void *st, const void *msg, size_t size)
{
	struct counter_state *s = st;
	(void)msg; (void)size;
	s->count++;
	return XTC_SVR_CONTINUE;
}

static int g_info_seen;
static int
counter_handle_info(void *st, const void *msg, size_t size)
{
	(void)st; (void)msg; (void)size;
	__os_atomic_fetch_add_i32(&g_info_seen, 1);
	return XTC_SVR_CONTINUE;
}

/* Driver proc that exercises the server, then stops it. */
static xtc_svr_t *g_svr_for_driver;

struct driver_args {
	xtc_pid_t target;
	_Atomic int call_result;
};

static void
driver_proc(void *arg)
{
	struct driver_args *da = arg;
	xtc_pid_t target = da->target;
	void  *reply;
	size_t reply_size;
	int    rc;
	int    val;

	/* Cast a bunch. */
	(void)xtc_svr_cast(target, NULL, 0);
	(void)xtc_svr_cast(target, NULL, 0);
	(void)xtc_svr_cast(target, NULL, 0);

	/* Send a raw message -- should hit handle_info. */
	{ uint8_t raw[2] = { 'Z', 0 };
	  (void)xtc_send(target, raw, sizeof raw); }

	/* Yield once so the server has a chance to drain its mailbox. */
	{ void *m; size_t s;
	  (void)xtc_recv(&m, &s, 50 * 1000 * 1000);
	  if (m) __os_free(m); }

	/* Synchronous call. */
	rc = xtc_svr_call(target, NULL, 0, &reply, &reply_size,
	    1000LL * 1000 * 1000);
	if (rc == XTC_OK && reply_size == sizeof(int)) {
		memcpy(&val, reply, sizeof val);
		__os_free(reply);
	} else {
		val = -1;
	}
	atomic_store_explicit(&da->call_result, val, memory_order_release);

	(void)xtc_svr_stop(g_svr_for_driver);
}

static MunitResult
test_svr_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	xtc_svr_callbacks_t cb = {
		.init        = NULL,
		.handle_call = counter_handle_call,
		.handle_cast = counter_handle_cast,
		.handle_info = counter_handle_info,
		.terminate   = NULL
	};
	xtc_svr_opts_t opts = { .name = "counter", .mailbox_cap = 0 };
	struct counter_state state = {0};
	struct driver_args da;
	xtc_pid_t dpid;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_info_seen, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, &state, &opts, &svr),
	    ==, XTC_OK);
	g_svr_for_driver = svr;

	da.target = xtc_svr_pid(svr);
	atomic_store_explicit(&da.call_result, -2, memory_order_relaxed);
	munit_assert_int(xtc_proc_spawn(loop, driver_proc, &da, NULL,
	    &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Three casts incremented the counter to 3.  The synchronous
	 * call read 3 back. */
	munit_assert_int(state.count, ==, 3);
	munit_assert_int(atomic_load_explicit(&da.call_result,
	    memory_order_acquire), ==, 3);
	/* The raw-send hit handle_info exactly once. */
	munit_assert_int(__os_atomic_load_i32(&g_info_seen), ==, 1);

	munit_assert_int(xtc_svr_join(svr, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- abortable call: a server that never replies, a token fired by
 * a peer; the in-proc call must return XTC_E_ABORTED (cooperative
 * cancellation -- the PG statement-timeout lever). ---- */
static int
blackhole_handle_call(void *st, const void *req, size_t size,
                      xtc_svr_call_t *call)
{
	(void)st; (void)req; (void)size; (void)call;
	return XTC_SVR_CONTINUE;            /* never replies */
}

struct abrt_args {
	xtc_pid_t           target;
	xtc_abort_source_t *src;
	xtc_abort_token_t   tok;
	_Atomic int         rc;
};
static xtc_svr_t *g_blackhole_svr;

static void
abrt_caller(void *arg)
{
	struct abrt_args *a = arg;
	void *reply = NULL; size_t rsz = 0;
	int rc = xtc_svr_call_abortable(a->target, NULL, 0, &reply, &rsz,
	    5LL * 1000 * 1000 * 1000, &a->tok);
	atomic_store_explicit(&a->rc, rc, memory_order_release);
	if (reply) __os_free(reply);
	(void)xtc_svr_stop(g_blackhole_svr);
}
static void
abrt_canceller(void *arg)
{
	struct abrt_args *a = arg;
	void *m = NULL; size_t n = 0;
	(void)xtc_recv(&m, &n, 60LL * 1000 * 1000);   /* let caller park */
	if (m) __os_free(m);
	(void)xtc_abort_source_fire(a->src, 9);
}

static MunitResult
test_svr_call_abortable(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	xtc_svr_callbacks_t cb = {
		.init = NULL, .handle_call = blackhole_handle_call,
		.handle_cast = NULL, .handle_info = NULL, .terminate = NULL
	};
	xtc_svr_opts_t opts = { .name = "blackhole", .mailbox_cap = 0 };
	struct abrt_args a;
	xtc_pid_t cpid, kpid;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, NULL, &opts, &svr),
	    ==, XTC_OK);
	g_blackhole_svr = svr;

	memset(&a, 0, sizeof a);
	a.target = xtc_svr_pid(svr);
	munit_assert_int(xtc_abort_source_create(&a.src), ==, XTC_OK);
	munit_assert_int(xtc_abort_source_token(a.src, &a.tok), ==, XTC_OK);
	atomic_store_explicit(&a.rc, 0, memory_order_relaxed);

	munit_assert_int(xtc_proc_spawn(loop, abrt_caller, &a, NULL, &cpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, abrt_canceller, &a, NULL, &kpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load_explicit(&a.rc, memory_order_acquire),
	    ==, XTC_E_ABORTED);

	xtc_abort_source_destroy(a.src);
	munit_assert_int(xtc_svr_join(svr, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- deferred reply (xtc_svr_call_save + XTC_SVR_NOREPLY): a server
 * that stashes calls and replies to the whole batch later, from a
 * cast.  This is the gen_server:reply/2 pattern the group-commit WAL
 * writer and the 2PC coordinator need. ---- */
#define DEFER_MAGIC 0xBEEF
struct defer_state {
	xtc_svr_call_t *saved[4];
	int             n;
};
static int
defer_handle_call(void *st, const void *req, size_t size, xtc_svr_call_t *call)
{
	struct defer_state *s = st;
	(void)req; (void)size;
	/* Stash a heap-survivable handle and answer later. */
	s->saved[s->n++] = xtc_svr_call_save(call);
	return XTC_SVR_NOREPLY;
}
static int
defer_handle_cast(void *st, const void *msg, size_t size)
{
	struct defer_state *s = st;
	int v = DEFER_MAGIC, i;
	(void)msg; (void)size;
	/* Flush: reply to every deferred caller (frees each saved handle). */
	for (i = 0; i < s->n; i++)
		(void)xtc_svr_reply(s->saved[i], &v, sizeof v);
	s->n = 0;
	return XTC_SVR_CONTINUE;
}

struct defer_args { xtc_pid_t target; _Atomic int rc; _Atomic int val; };
static xtc_svr_t *g_defer_svr;
static _Atomic int g_defer_left;

static void
defer_caller(void *arg)
{
	struct defer_args *a = arg;
	void *reply = NULL; size_t rsz = 0;
	int rc = xtc_svr_call(a->target, NULL, 0, &reply, &rsz,
	    5LL * 1000 * 1000 * 1000);
	atomic_store_explicit(&a->rc, rc, memory_order_release);
	if (rc == XTC_OK && rsz == sizeof(int))
		atomic_store_explicit(&a->val, *(int *)reply, memory_order_release);
	if (reply) __os_free(reply);
	/* The last caller to get its deferred reply stops the server. */
	if (atomic_fetch_sub(&g_defer_left, 1) == 1)
		(void)xtc_svr_stop(g_defer_svr);
}
static void
defer_flusher(void *arg)
{
	struct defer_args *a = arg;
	(void)xtc_proc_sleep(10LL * 1000 * 1000);   /* let both calls register */
	(void)xtc_svr_cast(a->target, "flush", 5);
}

static MunitResult
test_svr_deferred_reply(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	xtc_svr_callbacks_t cb = {
		.init = NULL, .handle_call = defer_handle_call,
		.handle_cast = defer_handle_cast, .handle_info = NULL,
		.terminate = NULL
	};
	xtc_svr_opts_t opts = { .name = "defer", .mailbox_cap = 0 };
	struct defer_state state = {0};
	struct defer_args a1, a2, fa;
	xtc_pid_t p1, p2, pf;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, &state, &opts, &svr), ==, XTC_OK);
	g_defer_svr = svr;
	atomic_store(&g_defer_left, 2);

	memset(&a1, 0, sizeof a1); memset(&a2, 0, sizeof a2); memset(&fa, 0, sizeof fa);
	a1.target = a2.target = fa.target = xtc_svr_pid(svr);

	munit_assert_int(xtc_proc_spawn(loop, defer_caller, &a1, NULL, &p1), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, defer_caller, &a2, NULL, &p2), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, defer_flusher, &fa, NULL, &pf), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Both calls deferred past handle_call, then answered from the
	 * cast: each caller got the magic reply. */
	munit_assert_int(atomic_load(&a1.rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&a2.rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&a1.val), ==, DEFER_MAGIC);
	munit_assert_int(atomic_load(&a2.val), ==, DEFER_MAGIC);

	munit_assert_int(xtc_svr_join(svr, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* handle_continue: init returns fast after arming a continuation; the
 * continuation runs before the first message and marks state ready. */
struct cont_state { _Atomic int inited; _Atomic int continued; _Atomic int order_bad; };
static int
cont_init(void *st)
{
	struct cont_state *s = st;
	atomic_store(&s->inited, 1);
	/* the continuation must not have run yet */
	if (atomic_load(&s->continued)) atomic_store(&s->order_bad, 1);
	return xtc_svr_continue(s);   /* arm; returns XTC_OK */
}
static int
cont_continue(void *st, void *cont)
{
	struct cont_state *s = st;
	(void)cont;   /* == s */
	if (!atomic_load(&s->inited)) atomic_store(&s->order_bad, 1);
	atomic_store(&s->continued, 1);
	return XTC_OK;
}
static int
cont_call(void *st, const void *req, size_t n, xtc_svr_call_t *call)
{
	struct cont_state *s = st;
	int ready = atomic_load(&s->continued);   /* must be 1 by first call */
	(void)req; (void)n;
	(void)xtc_svr_reply(call, &ready, sizeof ready);
	return XTC_SVR_CONTINUE;
}
static const xtc_svr_callbacks_t CONT_CB = {
	cont_init, cont_call, NULL, NULL, cont_continue, NULL
};
struct cont_drv { xtc_svr_t *svr; int reply_ready; };
static void
cont_driver(void *arg)
{
	struct cont_drv *d = arg;
	void *rep = NULL; size_t rn = 0;
	if (xtc_svr_call(xtc_svr_pid(d->svr), "go", 2, &rep, &rn,
	    1000LL * 1000 * 1000) == XTC_OK && rn == sizeof(int))
		memcpy(&d->reply_ready, rep, sizeof(int));
	if (rep) xtc_free(rep);
	(void)xtc_svr_stop(d->svr);
}
static MunitResult
test_svr_handle_continue(const MunitParameter p[], void *dp)
{
	xtc_loop_t *loop = NULL;
	struct cont_state st;
	struct cont_drv d;
	(void)p; (void)dp;
	memset(&st, 0, sizeof st); memset(&d, 0, sizeof d);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &CONT_CB, &st, NULL, &d.svr), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, cont_driver, &d, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&st.continued), ==, 1);
	munit_assert_int(atomic_load(&st.order_bad), ==, 0);  /* init before continue before call */
	munit_assert_int(d.reply_ready, ==, 1);               /* continuation ran before first call */
	(void)xtc_svr_join(d.svr, 0);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ---- xtc_svr_call_name: address the server by registered name via a
 * registry, instead of by pid (the {via, ...}/global-name pattern). ---- */
static xtc_reg_t  *g_cn_reg;
static xtc_svr_t  *g_cn_svr;

struct call_name_args {
	_Atomic int result;
	_Atomic int miss_rc;
};

static void
call_name_driver(void *arg)
{
	struct call_name_args *a = arg;
	void  *reply = NULL;
	size_t reply_size = 0;
	int    val = -1, rc;

	/* Bump the counter so the read returns something nonzero. */
	{
		xtc_pid_t p;
		if (xtc_reg_whereis(g_cn_reg, "counter.named", &p) == XTC_OK) {
			(void)xtc_svr_cast(p, NULL, 0);
			(void)xtc_svr_cast(p, NULL, 0);
		}
	}
	/* Let the casts drain. */
	{ void *m; size_t s; (void)xtc_recv(&m, &s, 50 * 1000 * 1000);
	  if (m) __os_free(m); }

	/* Call by name. */
	rc = xtc_svr_call_name(g_cn_reg, "counter.named", NULL, 0,
	    &reply, &reply_size, 1000LL * 1000 * 1000);
	if (rc == XTC_OK && reply_size == sizeof(int)) {
		memcpy(&val, reply, sizeof val);
		__os_free(reply);
	}
	atomic_store(&a->result, val);

	/* Unknown name -> XTC_E_NOTFOUND (registry miss). */
	atomic_store(&a->miss_rc,
	    xtc_svr_call_name(g_cn_reg, "no.such.server", NULL, 0,
	        &reply, &reply_size, 1000LL * 1000 * 1000));

	(void)xtc_svr_stop(g_cn_svr);
}

static MunitResult
test_svr_call_name(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_callbacks_t cb = {
		.init        = NULL,
		.handle_call = counter_handle_call,
		.handle_cast = counter_handle_cast,
		.handle_info = counter_handle_info,
		.terminate   = NULL
	};
	xtc_svr_opts_t opts = { .name = "counter.named", .mailbox_cap = 0 };
	struct counter_state state = { 0 };
	struct call_name_args a;
	xtc_pid_t dpid;
	(void)p; (void)d;

	munit_assert_int(xtc_reg_create(&g_cn_reg), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, &state, &opts, &g_cn_svr),
	    ==, XTC_OK);
	munit_assert_int(xtc_reg_register(g_cn_reg, "counter.named",
	    xtc_svr_pid(g_cn_svr)), ==, XTC_OK);

	atomic_store(&a.result, -2);
	atomic_store(&a.miss_rc, 12345);
	munit_assert_int(xtc_proc_spawn(loop, call_name_driver, &a, NULL,
	    &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Two casts -> counter == 2, read back by name. */
	munit_assert_int(atomic_load(&a.result), ==, 2);
	munit_assert_int(atomic_load(&a.miss_rc), ==, XTC_E_NOTFOUND);

	munit_assert_int(xtc_svr_join(g_cn_svr, 1LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	xtc_reg_destroy(g_cn_reg);
	return MUNIT_OK;
}


/* ---- xtc_svr_call from a PLAIN OS THREAD (the off-loop branch) --------
 *
 * xtc_svr_call routes on whether the caller is a proc:
 *     if (!xtc_pid_is_none(xtc_self()))  -> in-proc path ('Cp' message,
 *                                           reply delivered to the pid)
 *     else                               -> OFF-LOOP path ('Cs' message,
 *                                           reply handed back through a
 *                                           condvar-guarded slot)
 * Every existing case calls from inside a fiber, so the whole off-loop
 * half of svr.c -- the slot construction, its mutex/notify lifecycle, the
 * 'Cs' encoding, and the reply hand-back -- was never executed.  That is
 * the path an embedder uses when ordinary application threads talk to a
 * gen_server, so it is worth a real test.
 *
 * The loop must be RUNNING while the foreign thread calls, so the thread
 * is started first and the server is stopped from the callback once it
 * has served the call (otherwise xtc_loop_run would never return).
 */
struct offloop_args {
	xtc_pid_t target;
	_Atomic int rc;
	_Atomic int value;
	_Atomic int started;
};

static struct offloop_args *g_ol;
static xtc_svr_t *g_ol_svr;
static xtc_loop_t *g_ol_loop;

/* Server callback: serve the value, then ask the loop to stop so the
 * test terminates deterministically. */
static int
ol_handle_call(void *st, const void *req, size_t size, xtc_svr_call_t *call)
{
	int v = 4242;
	(void)st; (void)req; (void)size;
	(void)xtc_svr_reply(call, &v, sizeof v);
	return XTC_SVR_STOP;
}

static void *
ol_thread(void *arg)
{
	struct offloop_args *a = arg;
	void  *reply = NULL;
	size_t rsz = 0;
	int rc;

	/* Wait until the server pid is published and the loop is running. */
	while (!atomic_load(&a->started))
		(void)xtc_sleep_ns(1000 * 1000LL);

	/* xtc_self() is NONE here (not a proc), so this takes the off-loop
	 * branch. */
	/* The caller sets a->started BEFORE it enters xtc_loop_run, so this
	 * off-loop call can be issued while the loop is not yet running and
	 * the server proc not yet scheduled -- hence a deadline generous
	 * enough for a slow shared runner, but BOUNDED. */
	rc = xtc_svr_call(a->target, "q", 1, &reply, &rsz,
	    10LL * 1000 * 1000 * 1000);
	atomic_store(&a->rc, rc);
	if (rc == XTC_OK && reply != NULL && rsz == sizeof(int)) {
		int v;
		memcpy(&v, reply, sizeof v);
		atomic_store(&a->value, v);
	}
	xtc_free(reply);
	/* The loop is stopped by ol_handle_call returning XTC_SVR_STOP -- but
	 * ONLY if the call actually reached the handler.  If it timed out, no
	 * handler ran, nothing stops the loop, and xtc_loop_run would block
	 * FOREVER: the test would hang instead of failing, which is strictly
	 * worse (it burned a 45-minute CI job once).  Always stop the loop
	 * from here so a failure is a clean assertion on a->rc below. */
	(void)xtc_loop_stop(g_ol_loop);
	return NULL;
}

static MunitResult
test_svr_call_off_loop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_svr_t  *svr = NULL;
	xtc_svr_callbacks_t cb = {
		.init = NULL, .handle_call = ol_handle_call,
		.handle_cast = NULL, .handle_info = NULL, .terminate = NULL
	};
	xtc_svr_opts_t opts = { .name = "offloop", .mailbox_cap = 0 };
	struct offloop_args a;
	pthread_t th;
	(void)p; (void)d;

	memset(&a, 0, sizeof a);
	atomic_store(&a.rc, -12345);
	atomic_store(&a.value, 0);
	atomic_store(&a.started, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, NULL, &opts, &svr),
	    ==, XTC_OK);
	g_ol_svr = svr;
	g_ol_loop = loop;
	a.target = xtc_svr_pid(svr);
	g_ol = &a;

	munit_assert_int(pthread_create(&th, NULL, ol_thread, &a), ==, 0);
	atomic_store(&a.started, 1);

	/* Runs until ol_handle_call returns XTC_SVR_STOP. */
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(pthread_join(th, NULL), ==, 0);

	munit_assert_int(atomic_load(&a.rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&a.value), ==, 4242);

	/* Reap the server before tearing down the loop: xtc_svr_join is what
	 * releases the svr object (the handler returned XTC_SVR_STOP, so it
	 * has already exited and this does not block).  Omitting it leaks the
	 * svr and its state -- 216 bytes LeakSanitizer fails the run on, which
	 * is exactly how this was caught.  Every sibling test in this file
	 * joins; this one did not. */
	munit_assert_int(xtc_svr_join(svr, 1LL * 1000 * 1000 * 1000), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/svr_basic", test_svr_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_name", test_svr_call_name, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_abortable", test_svr_call_abortable, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/deferred_reply", test_svr_deferred_reply, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/handle_continue", test_svr_handle_continue, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_off_loop",  test_svr_call_off_loop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/svr", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
