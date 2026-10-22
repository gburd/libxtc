/*
 * docs/_includes/snippets/16_usdt_target.c
 *
 * A program to point DTrace, perf or bpftrace at.  It needs no tracing
 * code of its own: the probes are in libxtc (configure --enable-dtrace).
 * Two fibers bounce a message and contend on one lock, so the park,
 * wake, message and lock-wait probes all fire.
 *
 * Compiled AND run by test/docs/test_doc_snippets.sh (a release gate):
 * it must exit 0, with or without probes in the library.
 */
#include <stdio.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

/* !region full */
static xtc_amutex_t *mu;
static xtc_pid_t     peer;

static void
pong(void *arg)
{
	void *m;
	size_t sz;
	(void)arg;
	while (xtc_recv(&m, &sz, 100 * 1000 * 1000LL) == XTC_OK) {
		xtc_free(m);
		(void)xtc_amutex_lock(mu, -1);     /* waits: ping holds it */
		(void)xtc_amutex_unlock(mu);
	}
}

static void
ping(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 100; i++) {
		(void)xtc_amutex_lock(mu, -1);
		(void)xtc_send(peer, &i, sizeof i);
		(void)xtc_proc_sleep(100 * 1000);  /* hold the lock 100 us */
		(void)xtc_amutex_unlock(mu);
		(void)xtc_proc_sleep(100 * 1000);
	}
}

int
main(void)
{
	xtc_loop_t *loop;

	if (xtc_amutex_create(&mu) != XTC_OK || xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, pong, NULL, NULL, &peer) != XTC_OK ||
	    xtc_proc_spawn(loop, ping, NULL, NULL, NULL) != XTC_OK)
		return 1;
	if (xtc_loop_run(loop) != XTC_OK)
		return 1;
	(void)xtc_loop_fini(loop);
	xtc_amutex_destroy(mu);
	puts("done");
	return 0;
}
/* !endregion full */
