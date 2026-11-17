/*
 * docs snippet: a saga whose steps share a pooled connection (examples
 * 17_checkout).  Two steps succeed, the third fails, and the saga undoes
 * the first two in reverse.  Exits 0 iff the compensations ran in that
 * order and the connection went back to the pool.
 */
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_pool.h"
#include "xtc_proc.h"
#include "xtc_saga.h"

#define EXPECT "reserve charge ship:no-carrier refund release "

static char trail[64];
static int ok;

static void
note(const char *s)
{
	strncat(trail, s, sizeof trail - strlen(trail) - 1);
}

/* The steps.  Each records that it ran; ship fails. */
static int
reserve(void *c)
{
	(void)c;
	note("reserve ");
	return XTC_OK;
}
static int
release(void *c)
{
	(void)c;
	note("release ");
	return XTC_OK;
}
static int
charge(void *c)
{
	(void)c;
	note("charge ");
	return XTC_OK;
}
static int
refund(void *c)
{
	(void)c;
	note("refund ");
	return XTC_OK;
}
static int
ship(void *c)
{
	(void)c;
	note("ship:no-carrier ");
	return XTC_E_INVAL;
}

/* !region saga */
static void
order(void *arg)
{
	xtc_pool_t *pool = arg;
	xtc_saga_t *saga = NULL;
	void *conn = NULL;
	int rc;

	/* Parks this fiber, not the loop, until a connection is free. */
	if (xtc_pool_checkout(pool, -1, &conn) != XTC_OK)
		return;
	(void)xtc_saga_create(&saga);
	(void)xtc_saga_step(saga, reserve, release, conn);
	(void)xtc_saga_step(saga, charge, refund, conn);
	(void)xtc_saga_step(saga, ship, NULL, conn); /* last: no undo */

	rc = xtc_saga_run(saga); /* ship fails: refund, then release */
	printf("rc=%d failed at step %d: %s\n", rc, xtc_saga_failed_step(saga),
	    trail);
	ok = rc == XTC_E_INVAL && xtc_saga_failed_step(saga) == 2 &&
	    strcmp(trail, EXPECT) == 0;
	xtc_saga_destroy(saga);
	(void)xtc_pool_checkin(pool, conn);
}
/* !endregion saga */

int
main(void)
{
	static int conns[2];
	xtc_loop_t *loop;
	xtc_pool_t *pool;

	if (xtc_loop_create(&loop) != XTC_OK ||
	    xtc_pool_create(2, &pool) != XTC_OK)
		return 1;
	(void)xtc_pool_add(pool, &conns[0]);
	(void)xtc_pool_add(pool, &conns[1]);
	if (xtc_proc_spawn(loop, order, pool, NULL, NULL) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	xtc_loop_destroy(loop);
	ok = ok && xtc_pool_available(pool) == 2;
	xtc_pool_destroy(pool);
	return ok ? 0 : 1;
}
