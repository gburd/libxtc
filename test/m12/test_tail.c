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

struct counts { int spawn; int exit; int exit7; };

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

/* Binary dump: header magic/version/rec_size/count round-trip via a pipe. */
static MunitResult
test_tail_dump(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	int fds[2];
	xtc_tail_hdr_t hdr;
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

	r = read(fds[0], &hdr, sizeof hdr);
	munit_assert_int((int)r, ==, (int)sizeof hdr);
	munit_assert_uint(hdr.magic, ==, XTC_TAIL_MAGIC);
	munit_assert_uint(hdr.version, ==, XTC_TAIL_VERSION);
	munit_assert_uint(hdr.rec_size, ==, (uint32_t)sizeof(xtc_tail_rec_t));
	munit_assert_uint(hdr.count, ==, (uint32_t)n);

	/* First record body decodes as a real event. */
	{
		xtc_tail_rec_t rec;
		r = read(fds[0], &rec, sizeof rec);
		munit_assert_int((int)r, ==, (int)sizeof rec);
		munit_assert_uint(rec.source, ==, XTC_TAIL_SCHED);
	}
	close(fds[0]);
	xtc_tail_disable();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/sched",    test_tail_sched,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/disabled", test_tail_disabled, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump",     test_tail_dump,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m12/tail", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
