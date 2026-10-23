/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m10/test_svr_copies.c
 *	Payload copies on the gen_server / gen_statem call paths.
 *
 *	An in-proc xtc_svr_call / xtc_fsm_call round trip of a P-byte
 *	request and an R-byte reply must make exactly ONE payload-sized
 *	allocation per direction (the framed message the caller resp. the
 *	server builds and then moves with xtc_send_owned), and the caller
 *	must be handed the very block the server built (no strip copy).
 *	Before zero-copy messaging each direction took three or four:
 *	frame, the mailbox envelope copy, the xtc_recv copy, and (reply)
 *	the header-strip copy.
 *
 *	The counting allocator hook records mallocs whose size is within
 *	64 bytes above the payload size (frames carry <= 18 header bytes),
 *	so unrelated small allocations do not count.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_svr.h"
#include "xtc_fsm.h"
#include "os_alloc.h"

#define REQ_SZ   40000u
#define REPLY_SZ 50000u

static struct __os_alloc_hook g_down;
static _Atomic int g_req_allocs, g_reply_allocs;

static void
count(size_t n)
{
	if (n >= REQ_SZ && n < REQ_SZ + 64)
		atomic_fetch_add(&g_req_allocs, 1);
	if (n >= REPLY_SZ && n < REPLY_SZ + 64)
		atomic_fetch_add(&g_reply_allocs, 1);
}
static void *c_malloc(size_t n) { count(n); return g_down.malloc(n); }
static void *c_calloc(size_t k, size_t n) { count(k * n); return g_down.calloc(k, n); }
static void *c_realloc(void *q, size_t n) { count(n); return g_down.realloc(q, n); }
static void c_free(void *q) { g_down.free(q); }
static void *c_aligned(size_t a, size_t n) { return g_down.aligned(a, n); }
static void c_aligned_free(void *q) { g_down.aligned_free(q); }

static void
hook_on(void)
{
	struct __os_alloc_hook h;
	munit_assert_int(__os_alloc_get_hook(&g_down), ==, XTC_OK);
	h.malloc = c_malloc;
	h.calloc = c_calloc;
	h.realloc = c_realloc;
	h.free = c_free;
	h.aligned = c_aligned;
	h.aligned_free = c_aligned_free;
	munit_assert_int(__os_alloc_set_hook(&h), ==, XTC_OK);
}

static void
hook_off(void)
{
	(void)__os_alloc_set_hook(&g_down);
}

static uint8_t g_req[REQ_SZ], g_rep[REPLY_SZ];
static int g_req_ok, g_rep_ok;
static int g_req_allocs_at_call, g_rep_allocs_at_reply;

/* ---- gen_server ---------------------------------------------------- */

static int
svr_call_cb(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	(void)st;
	g_req_ok = (sz == REQ_SZ && memcmp(req, g_req, sz) == 0);
	g_req_allocs_at_call = atomic_load(&g_req_allocs);
	(void)xtc_svr_reply(call, g_rep, sizeof g_rep);
	return XTC_SVR_STOP;
}

static const xtc_svr_callbacks_t svr_cb = {
	.init = NULL, .handle_call = svr_call_cb,
	.handle_cast = NULL, .handle_info = NULL, .terminate = NULL
};

static void
svr_caller(void *arg)
{
	xtc_svr_t *s = arg;
	void *r = NULL;
	size_t n = 0;
	if (xtc_svr_call(xtc_svr_pid(s), g_req, sizeof g_req, &r, &n,
	    5LL * 1000 * 1000 * 1000) != XTC_OK)
		return;
	g_rep_allocs_at_reply = atomic_load(&g_reply_allocs);
	g_rep_ok = (n == REPLY_SZ && memcmp(r, g_rep, n) == 0);
	xtc_free(r);
}

static MunitResult
test_svr_call_copies(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t *s;
	(void)p; (void)d;
	memset(g_req, 0x11, sizeof g_req);
	memset(g_rep, 0x22, sizeof g_rep);
	g_req_ok = g_rep_ok = 0;
	atomic_store(&g_req_allocs, 0);
	atomic_store(&g_reply_allocs, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &svr_cb, NULL, NULL, &s), ==,
	    XTC_OK);
	hook_on();
	munit_assert_int(xtc_proc_spawn(loop, svr_caller, s, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	hook_off();
	munit_assert_int(xtc_svr_join(s, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	munit_assert_int(g_req_ok, ==, 1);
	munit_assert_int(g_rep_ok, ==, 1);
	munit_assert_int(g_req_allocs_at_call, ==, 1);
	munit_assert_int(g_rep_allocs_at_reply, ==, 1);
	return MUNIT_OK;
}

/* ---- gen_statem ---------------------------------------------------- */

static xtc_fsm_result_t
fsm_event(void *st, int cur, const void *msg, size_t len,
    xtc_fsm_call_t *call)
{
	xtc_fsm_result_t r = { XTC_FSM_STOP, 0, 0 };
	(void)st; (void)cur;
	if (call == NULL)
		return r;
	g_req_ok = (len == REQ_SZ && memcmp(msg, g_req, len) == 0);
	g_req_allocs_at_call = atomic_load(&g_req_allocs);
	(void)xtc_fsm_reply(call, g_rep, sizeof g_rep);
	return r;
}

static const xtc_fsm_callbacks_t fsm_cb = { NULL, fsm_event, NULL };

static void
fsm_caller(void *arg)
{
	xtc_fsm_t *f = arg;
	void *r = NULL;
	size_t n = 0;
	if (xtc_fsm_call(xtc_fsm_pid(f), g_req, sizeof g_req, &r, &n,
	    5LL * 1000 * 1000 * 1000) != XTC_OK)
		return;
	g_rep_allocs_at_reply = atomic_load(&g_reply_allocs);
	g_rep_ok = (n == REPLY_SZ && memcmp(r, g_rep, n) == 0);
	xtc_free(r);
}

static MunitResult
test_fsm_call_copies(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_fsm_t *f;
	(void)p; (void)d;
	memset(g_req, 0x33, sizeof g_req);
	memset(g_rep, 0x44, sizeof g_rep);
	g_req_ok = g_rep_ok = 0;
	atomic_store(&g_req_allocs, 0);
	atomic_store(&g_reply_allocs, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_fsm_start(loop, &fsm_cb, NULL, 0, NULL, &f), ==,
	    XTC_OK);
	hook_on();
	munit_assert_int(xtc_proc_spawn(loop, fsm_caller, f, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	hook_off();
	munit_assert_int(xtc_fsm_join(f, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	munit_assert_int(g_req_ok, ==, 1);
	munit_assert_int(g_rep_ok, ==, 1);
	munit_assert_int(g_req_allocs_at_call, ==, 1);
	munit_assert_int(g_rep_allocs_at_reply, ==, 1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/svr_call", test_svr_call_copies, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fsm_call", test_fsm_call_copies, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10/svr_copies", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
