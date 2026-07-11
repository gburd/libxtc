/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_tail.c
 *	The runtime microscope (src/ptc/tail.c): the SCHED source records
 *	proc spawn/exit events; xtc_tail_read visits them; xtc_tail_dump
 *	writes a versioned binary trace whose header round-trips.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_tail.h"

/* A proc that exits with a fixed reason so EXIT.detail is predictable. */
static void
exit7_proc(void *arg)
{
	(void)arg;
	xtc_exit_self(7);
}

struct counts { int spawn; int exit; int exit7; int park; int run; int run_latency_seen; };

static int
count_cb(const xtc_tail_rec_t *r, void *user)
{
	struct counts *c = user;
	if (r->source != XTC_TAIL_SCHED) return 0;
	if (r->kind == XTC_TAIL_SPAWN) c->spawn++;
	if (r->kind == XTC_TAIL_EXIT) {
		c->exit++;
		if (r->detail == 7) c->exit7++;
	}
	if (r->kind == XTC_TAIL_PARK) c->park++;
	if (r->kind == XTC_TAIL_RUN) {
		c->run++;
		if (r->detail > 0) c->run_latency_seen++;   /* park->run ns */
	}
	return 0;
}

static MunitResult
test_tail_sched(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct counts c;
	unsigned prev;
	(void)p; (void)d;

	(void)xtc_tail_reset();
	prev = xtc_tail_enable(XTC_TAIL_SCHED);
	munit_assert_uint(prev, ==, 0);   /* was off */

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Spawn three procs that each exit with reason 7. */
	munit_assert_int(xtc_proc_spawn(loop, exit7_proc, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, exit7_proc, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, exit7_proc, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	memset(&c, 0, sizeof c);
	munit_assert_int(xtc_tail_read(count_cb, &c), ==, XTC_OK);
	munit_assert_int(c.spawn, ==, 3);    /* three spawns recorded */
	munit_assert_int(c.exit,  ==, 3);    /* three exits recorded */
	munit_assert_int(c.exit7, ==, 3);    /* each with reason 7 */
	munit_assert_size(xtc_tail_count(), >=, 6);

	xtc_tail_disable();
	return MUNIT_OK;
}

/* Disabled source records nothing (the fast-path branch). */
static MunitResult
test_tail_disabled(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p; (void)d;

	(void)xtc_tail_reset();
	xtc_tail_disable();
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, exit7_proc, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_size(xtc_tail_count(), ==, 0);   /* nothing recorded */
	return MUNIT_OK;
}

/* Binary dump: portable LE header + LEB128 events round-trip via a pipe. */
static uint64_t
rd_le32(const uint8_t *b) { return (uint64_t)b[0] | ((uint64_t)b[1]<<8) |
    ((uint64_t)b[2]<<16) | ((uint64_t)b[3]<<24); }

static MunitResult
test_tail_dump(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	int fds[2];
	uint8_t hdr[24];
	uint8_t body[512];
	size_t n;
	ssize_t r;
	(void)p; (void)d;

	(void)xtc_tail_reset();
	(void)xtc_tail_enable(XTC_TAIL_SCHED);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, exit7_proc, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	n = xtc_tail_count();
	munit_assert_size(n, >=, 2);

	munit_assert_int(pipe(fds), ==, 0);
	munit_assert_int(xtc_tail_dump(fds[1]), ==, XTC_OK);
	close(fds[1]);

	r = read(fds[0], hdr, sizeof hdr);
	munit_assert_int((int)r, ==, (int)sizeof hdr);
	munit_assert_uint((unsigned)rd_le32(hdr + 0), ==, XTC_TAIL_MAGIC);
	munit_assert_uint((unsigned)rd_le32(hdr + 4), ==, XTC_TAIL_VERSION);
	munit_assert_uint((unsigned)rd_le32(hdr + 8), ==, XTC_TAIL_FLAG_LE);
	munit_assert_uint((unsigned)rd_le32(hdr + 12), ==, (unsigned)n);

	/* First event body decodes: kind byte, source byte == SCHED. */
	r = read(fds[0], body, sizeof body);
	munit_assert_int((int)r, >, 2);   /* at least the two fixed bytes + varints */
	munit_assert_uint(body[1], ==, (uint8_t)XTC_TAIL_SCHED);  /* source */

	/* Compactness: the whole event stream must be far under the raw
	 * 32-byte-per-record size (portable varint encoding). */
	munit_assert_int((int)r, <, (int)(n * sizeof(xtc_tail_rec_t)));

	close(fds[0]);
	xtc_tail_disable();
	return MUNIT_OK;
}

/* Wake-latency: a receiver parks in recv; a sender sleeps then sends, so
 * the receiver's RUN event carries a real park->run latency. */
struct wl_ctx { xtc_pid_t recv_pid; int recv_got; };

static void
wl_receiver(void *a)
{
	struct wl_ctx *c = a;
	void *m = NULL; size_t n = 0;
	if (xtc_recv(&m, &n, 2000LL * 1000 * 1000) == XTC_OK)
		c->recv_got = 1;
	if (m) xtc_free(m);
}

static void
wl_sender(void *a)
{
	struct wl_ctx *c = a;
	int msg = 1;
	xtc_proc_sleep(30LL * 1000 * 1000);   /* 30ms: receiver parks first */
	(void)xtc_send(c->recv_pid, &msg, sizeof msg);
}

static MunitResult
test_tail_wake_latency(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct wl_ctx c;
	struct counts cnt;
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	(void)xtc_tail_reset();
	(void)xtc_tail_enable(XTC_TAIL_SCHED);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, wl_receiver, &c, NULL,
	    &c.recv_pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, wl_sender, &c, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(c.recv_got, ==, 1);
	memset(&cnt, 0, sizeof cnt);
	munit_assert_int(xtc_tail_read(count_cb, &cnt), ==, XTC_OK);
	munit_assert_int(cnt.park, >=, 1);              /* receiver parked */
	munit_assert_int(cnt.run,  >=, 1);              /* and resumed */
	munit_assert_int(cnt.run_latency_seen, >=, 1);  /* with real latency */

	xtc_tail_disable();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/sched",        test_tail_sched,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/disabled",     test_tail_disabled,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump",         test_tail_dump,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wake_latency", test_tail_wake_latency, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m12/tail", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
