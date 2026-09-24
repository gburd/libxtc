/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_net_sigpipe.c -- a send to a closed peer must RETURN an
 * error, not kill the host process with SIGPIPE.
 *
 *	Reproduced: xtc_xsend to an xproc child that had exited killed the
 *	parent with status 141 (128 + SIGPIPE); every example hid it with
 *	signal(SIGPIPE, SIG_IGN).  Each probe runs in a forked child with
 *	SIGPIPE explicitly at SIG_DFL, so a regression shows up as the
 *	child dying by SIGPIPE rather than as a skipped assertion.
 */

#if defined(_WIN32)
/* No SIGPIPE (and no fork) on Windows: nothing to test. */
int main(void) { return 77; }
#else
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_net.h"

/* Child exit codes: 0 = the send returned the expected failure. */
#define CH_SENT_OK   10   /* a send to a dead peer claimed success */
#define CH_SETUP     11

static int
probe_frame(int fd)
{
	char payload[64];
	memset(payload, 'x', sizeof payload);
	return xtc_net_send_frame(fd, payload, sizeof payload);
}

static int
probe_creds(int fd)
{
	return xtc_net_unix_send_creds(fd, "hello", 5);
}

/* Fork; in the child restore SIG_DFL, make a socketpair, close the peer,
 * send.  Returns the child's raw wait status. */
static int
run_in_child(int (*probe)(int))
{
	pid_t pid;
	int st = 0;
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		int sv[2], rc;
		(void)signal(SIGPIPE, SIG_DFL);
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
			_exit(CH_SETUP);
		(void)close(sv[1]);                 /* the peer is gone */
		rc = probe(sv[0]);
		_exit(rc == XTC_OK ? CH_SENT_OK : 0);
	}
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	return st;
}

static MunitResult
check(int st)
{
	if (WIFSIGNALED(st))
		munit_errorf("send to a closed peer killed the process with "
		    "signal %d (SIGPIPE is %d)", WTERMSIG(st), SIGPIPE);
	munit_assert_true(WIFEXITED(st));
	munit_assert_int(WEXITSTATUS(st), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_send_frame_closed_peer(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return check(run_in_child(probe_frame));
}

static MunitResult
test_send_creds_closed_peer(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return check(run_in_child(probe_creds));
}

static MunitTest tests[] = {
	{ "/send_frame_closed_peer", test_send_frame_closed_peer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/send_creds_closed_peer", test_send_creds_closed_peer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/net_sigpipe", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
#endif /* !_WIN32 */
