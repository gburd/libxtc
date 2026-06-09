/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_isolated.c
 *	Memory-isolation tier (a): an isolated worker is a separate OS
 *	process (real MMU isolation) reached by a framed request/reply
 *	over its control socket.  Spawns an upper-casing worker and
 *	round-trips a request through xtc_osproc_call / xtc_osproc_serve.
 */

#include "munit.h"
#include "xtc.h"

#if defined(_WIN32)
static MunitResult
test_isolated(const MunitParameter p[], void *d)
{ (void)p; (void)d; return MUNIT_SKIP; }
#else

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_osproc.h"
#include "xtc_int.h"

/* Child side: upper-case each request frame. */
static int
upcase_handler(const void *req, size_t rl, void **reply, size_t *reply_len,
               void *arg)
{
	char *r;
	size_t i;
	(void)arg;
	r = malloc(rl ? rl : 1);
	if (r == NULL) return XTC_E_NOMEM;
	for (i = 0; i < rl; i++) {
		char c = ((const char *)req)[i];
		r[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}
	*reply = r;
	*reply_len = rl;
	return XTC_OK;
}

static int
worker_main(int ctrl_fd, void *arg)
{
	(void)arg;
	return xtc_osproc_serve(ctrl_fd, upcase_handler, NULL) == XTC_OK ? 0 : 1;
}

static int g_result;   /* 1 = ok, negative = failure code */

static void
driver(void *arg)
{
	xtc_osproc_t *p = NULL;
	void  *reply = NULL;
	size_t reply_len = 0;
	int    rc;
	(void)arg;

	if (xtc_osproc_isolated_spawn("upcase", worker_main, NULL, &p) != XTC_OK) {
		g_result = -1;
		return;
	}
	rc = xtc_osproc_call(p, "hello", 5, &reply, &reply_len, 4096,
	    2000LL * 1000 * 1000);
	if (rc == XTC_OK && reply_len == 5 && reply != NULL &&
	    memcmp(reply, "HELLO", 5) == 0)
		g_result = 1;
	else
		g_result = -2;
	if (reply != NULL) __os_free(reply);

	(void)xtc_osproc_signal(p, SIGTERM);
	(void)xtc_osproc_wait(p, NULL, 1000LL * 1000 * 1000);
	xtc_osproc_destroy(p);
}

static MunitResult
test_isolated(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p; (void)d;
	g_result = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, driver, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(g_result, ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}
#endif

static MunitTest tests[] = {
	{ "/request_reply", test_isolated, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10/isolated", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
