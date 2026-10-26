/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_conn_sup.c
 *	A connection proc that is KILLED (xtc_exit_pid) must release what
 *	it holds exactly as a client disconnect does: its socket, its
 *	charged buffers, its --max-clients slot, and its engine handle --
 *	and with the handle the open transaction's snapshot pin (the
 *	version-GC horizon) and its row write intents.
 *
 *	The teardown was straight-line code at the end of conn_proc, so a
 *	kill unwound the fiber past it: the transaction stayed open
 *	forever, GC could not reclaim behind its snapshot, and the next
 *	writer of its row napped the whole intent wait (10 s) and then
 *	failed with a conflict.
 *
 *	Each scenario: a conn proc (as main.c spawns one) over a socketpair
 *	runs BEGIN, an UPDATE of row 1 (a row intent), and a SELECT with a
 *	~20 KB line (a pinned snapshot and a read buffer grown past its
 *	uncharged base).  Another handle then makes row 3 versions only
 *	that snapshot still needs (control: GC reclaims none).  Then the
 *	connection ends -- (a) the client closes the socket, the control
 *	path; (b) the proc is killed -- and, once its DOWN arrives:
 *	the client sees EOF, the fd count and the conn count are back to
 *	baseline, the memory budget is back to 0, GC reclaims the pinned
 *	versions, the uncommitted UPDATE is gone, and another writer takes
 *	row 1 at once (well under the 10 s intent wait).  (c) checks that a
 *	kill is delivered at a statement boundary on every execution path
 *	(see mid_statement).
 *
 *	usage: test_conn_sup [cases]   (default "abc")
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"
#include "db.h"
#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_inspect.h"
#include "xtc_io.h"
#include "xtc_proc.h"
#include "t_tmp.h"

/* conn.c's view of main.c's server: only the connection count. */
struct server {
	_Atomic int n;
};
void server_inc_conn(struct server *s) { atomic_fetch_add(&s->n, 1); }
void server_dec_conn(struct server *s) { atomic_fetch_sub(&s->n, 1); }

static struct server g_srv;
static char          g_path[256];
static xtc_exec_t   *g_exec;
static db_t         *g_db;
static xtc_res_t     g_res;
static _Atomic int   g_done;
static const char   *g_which = "abc";   /* argv[1]: the cases to run */
static int           g_slow = 1;        /* x20 under valgrind / emulation */

typedef struct cli {
	int    fd;
	size_t len;
	char   buf[65536];
} cli_t;

static int64_t
ms_since(int64_t t0)
{
	return (xtc_clock_mono() - t0) / 1000000;
}

static int
nfds(void)
{
	int fd, n = 0;
	for (fd = 0; fd < 4096; fd++)
		if (fcntl(fd, F_GETFD) != -1)
			n++;
	return n;
}

/* The next reply line (newline stripped) into out: 1, 0 on EOF, -1 on
 * an error or no line within timeout_ms. */
static int
cli_line(cli_t *c, char *out, size_t cap, int64_t timeout_ms)
{
	int64_t t0 = xtc_clock_mono();
	char *nl;
	ssize_t n;
	uint32_t rev;
	size_t k;

	for (;;) {
		if ((nl = memchr(c->buf, '\n', c->len)) != NULL) {
			k = (size_t)(nl - c->buf);
			snprintf(out, cap, "%.*s", (int)k, c->buf);
			memmove(c->buf, nl + 1, c->len - k - 1);
			c->len -= k + 1;
			return 1;
		}
		n = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len,
		    MSG_DONTWAIT);
		if (n > 0) {
			c->len += (size_t)n;
			continue;
		}
		if (n == 0)
			return 0;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (ms_since(t0) >= timeout_ms)
			return -1;
		rev = 0;
		(void)xtc_proc_wait_fd(c->fd, XTC_IO_READABLE | XTC_IO_HUP |
		    XTC_IO_ERR, 50LL * 1000 * 1000, &rev);
		if (rev & XTC_WAIT_MAILBOX)       /* a DOWN waiting: no spin */
			(void)xtc_proc_sleep(1000LL * 1000);
	}
}

/* Run one query; 0 once its {"done"} arrives, -1 on {"err"} or none. */
static int
cli_q(cli_t *c, const char *sql)
{
	char line[512], *req;
	size_t n, off = 0;
	ssize_t w;
	int rc;

	n = strlen(sql) + 16;
	if ((req = malloc(n)) == NULL)
		return -1;
	n = (size_t)snprintf(req, n, "{\"q\":\"%s\"}\n", sql);
	while (off < n) {
		w = send(c->fd, req + off, n - off, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (w > 0)
			off += (size_t)w;
		else
			(void)xtc_proc_sleep(1000LL * 1000);
	}
	free(req);
	while ((rc = cli_line(c, line, sizeof line, 5000)) == 1) {
		if (strncmp(line, "{\"done\"", 7) == 0)
			return 0;
		if (strncmp(line, "{\"err\"", 6) == 0) {
			fprintf(stderr, "  query error: %s\n", line);
			return -1;
		}
	}
	return -1;
}

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Wait up to timeout_ms for the DOWN of pid: 1 if it came. */
static int
await_down(xtc_pid_t pid, int64_t timeout_ms)
{
	int64_t t0 = xtc_clock_mono(), left;
	xtc_pid_t who;
	void *m;
	size_t n;
	int got = 0;

	while (!got && (left = timeout_ms - ms_since(t0)) > 0) {
		if (xtc_recv(&m, &n, left * 1000000) != XTC_OK)
			break;
		if (m != NULL && xtc_down_decode(m, n, &who, NULL) == XTC_OK &&
		    xtc_pid_eq(who, pid))
			got = 1;
		xtc_free(m);
	}
	return got;
}

static void
scenario(int kill, const char *name)
{
	static char big[24 * 1024];
	conn_opts_t o;
	xtc_pid_t pid;
	uint64_t ref;
	cli_t *c;
	sx_db *h = NULL;
	char line[256];
	int sv[2], fd0, down, eof = -9, pre = -9, post = -9, i, rc;
	int64_t t0, wait_ms = -1, v1 = -9, mem;

	CK(sx_open(":memory:", &h) == SX_OK);
	if (h == NULL)
		return;
	(void)xstore_autovacuum_set((struct xsql *)h, 0);
	(void)xstore_gc_run((struct xsql *)h);    /* earlier scenarios' debris */
	if ((c = calloc(1, sizeof *c)) == NULL) {
		g_fail = 1;
		sx_close(h);
		return;
	}
	fd0 = nfds();
	CK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	c->fd = sv[1];
	memset(&o, 0, sizeof o);
	o.fd = sv[0];
	o.db = g_db;
	o.res = &g_res;
	o.server = &g_srv;
	server_inc_conn(&g_srv);                  /* as the listener does */
	CK(conn_spawn(xtc_exec_loop(g_exec, 1), &o, &pid) == XTC_OK);
	CK(xtc_monitor(pid, &ref) == XTC_OK);

	CK(cli_line(c, line, sizeof line, 2000) == 1 &&
	    strncmp(line, "{\"hello\"", 8) == 0);
	CK(cli_q(c, "BEGIN") == 0);
	CK(cli_q(c, "UPDATE t SET v = 100 WHERE k = 1") == 0);
	memset(big, ' ', sizeof big - 1);
	memcpy(big, "SELECT count(*) FROM t", 22);
	CK(cli_q(c, big) == 0);
	CK(xtc_res_used(&g_res, XTC_RES_MEM_BYTES) > 0);   /* rbuf grew */

	/* Versions of row 3 that only the connection's snapshot needs. */
	for (i = 0; i < 4; i++)
		CK(sx_exec(h, "UPDATE t SET v = v + 1 WHERE k = 3", NULL) == SX_OK);
	pre = xstore_gc_run((struct xsql *)h);
	CK(pre == 0);                             /* control: the pin holds */

	if (kill) {
		CK(xtc_exit_pid(pid, 9) == XTC_OK);
	} else {
		close(sv[1]);
		c->fd = sv[1] = -1;
	}
	down = await_down(pid, 3000 * g_slow);
	CK(down);
	if (kill) {
		eof = cli_line(c, line, sizeof line, 1000);
		CK(eof == 0);                     /* the server closed its end */
		close(sv[1]);
	}
	CK(nfds() == fd0);
	CK(atomic_load(&g_srv.n) == 0);
	mem = xtc_res_used(&g_res, XTC_RES_MEM_BYTES);
	CK(mem == 0);
	post = xstore_gc_run((struct xsql *)h);
	CK(post > 0);                             /* the horizon moved */
	v1 = one(h, "SELECT v FROM t WHERE k = 1");
	CK(v1 == 1);                              /* the open txn rolled back */
	t0 = xtc_clock_mono();
	rc = sx_exec(h, "UPDATE t SET v = 7 WHERE k = 1", NULL);
	wait_ms = ms_since(t0);
	CK(rc == SX_OK);
	CK(wait_ms < 1000 * g_slow);              /* no intent left to wait out */
	CK(sx_exec(h, "UPDATE t SET v = 1 WHERE k = 1", NULL) == SX_OK);
	sx_close(h);
	free(c);
	printf("  %-12s down %d eof %d fds %d->%d conns %d mem %lld gc %d->%d "
	    "row1 %lld intent wait %lld ms\n", name, down, eof, fd0, nfds(),
	    atomic_load(&g_srv.n), (long long)mem, pre, post, (long long)v1,
	    (long long)wait_ms);
}

/*
 * (c) The statement is the unit of kill delivery on every path.  The
 * boss holds row 2's intent in an open transaction; the connection's
 * autocommit UPDATE of row 2 waits for it; the boss kills the
 * connection while it waits.  The kill must wait for the statement:
 * the proc is still alive 300 ms later, the UPDATE completes once the
 * holder commits, and only then does the proc exit -- and tear down
 * (EOF, conn slot), which a kill used to skip.
 */
static void
mid_statement(void)
{
	static const char upd[] = "{\"q\":\"UPDATE t SET v = 5 WHERE k = 2\"}\n";
	conn_opts_t o;
	xtc_proc_info_t pi = XTC_PROC_INFO_INIT;
	xtc_pid_t pid;
	uint64_t ref;
	cli_t *c;
	sx_db *h = NULL;
	char line[256];
	int sv[2], alive = -1, down_early, down, eof;
	int64_t v2 = -9, t0;
	uint64_t w0;

	CK(sx_open(":memory:", &h) == SX_OK);
	if (h == NULL || (c = calloc(1, sizeof *c)) == NULL) {
		g_fail = 1;
		return;
	}
	CK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	c->fd = sv[1];
	memset(&o, 0, sizeof o);
	o.fd = sv[0];
	o.db = g_db;
	o.res = &g_res;
	o.server = &g_srv;
	server_inc_conn(&g_srv);
	CK(conn_spawn(xtc_exec_loop(g_exec, 1), &o, &pid) == XTC_OK);
	CK(xtc_monitor(pid, &ref) == XTC_OK);
	CK(cli_line(c, line, sizeof line, 2000) == 1);

	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(h, "UPDATE t SET v = 50 WHERE k = 2", NULL) == SX_OK);
	w0 = xstore_intent_waits();
	CK(send(c->fd, upd, sizeof upd - 1, MSG_NOSIGNAL) ==
	    (ssize_t)(sizeof upd - 1));
	for (t0 = xtc_clock_mono(); xstore_intent_waits() == w0 &&
	    ms_since(t0) < 5000 * g_slow;)            /* it waits on row 2 */
		(void)xtc_proc_sleep(1000LL * 1000);
	CK(xstore_intent_waits() > w0);
	CK(xtc_exit_pid(pid, 9) == XTC_OK);
	down_early = await_down(pid, 300);
	if (xtc_proc_info(pid, &pi) == XTC_OK)
		alive = pi.alive;
	CK(!down_early);
	CK(alive == 1);
	CK(sx_exec(h, "COMMIT", NULL) == SX_OK);
	down = down_early || await_down(pid, 3000 * g_slow);
	CK(down);
	eof = cli_line(c, line, sizeof line, 1000);
	while (eof == 1)
		eof = cli_line(c, line, sizeof line, 1000);
	CK(eof == 0);
	close(sv[1]);
	CK(atomic_load(&g_srv.n) == 0);
	v2 = one(h, "SELECT v FROM t WHERE k = 2");
	CK(v2 == 5);                    /* the statement ran to its end */
	CK(sx_exec(h, "UPDATE t SET v = 1 WHERE k = 2", NULL) == SX_OK);
	sx_close(h);
	free(c);
	printf("  %-12s down at 300 ms %d alive %d down %d eof %d row2 %lld\n",
	    "mid-stmt", down_early, alive, down, eof, (long long)v2);
}

static void
boss(void *arg)
{
	(void)arg;
	if (strchr(g_which, 'a') != NULL)
		scenario(0, "client close");
	if (strchr(g_which, 'b') != NULL)
		scenario(1, "kill");
	if (strchr(g_which, 'c') != NULL)
		mid_statement();
	db_destroy(g_db);
	sx_storage_quiesce();
}

static void *
watchdog(void *arg)
{
	int64_t t0 = xtc_clock_mono();
	(void)arg;
	while (!atomic_load(&g_done)) {
		if (ms_since(t0) > 60000 * g_slow) {
			fprintf(stderr, "FAIL: test_conn_sup: hang\n");
			_exit(3);
		}
		(void)xtc_sleep_ns(50LL * 1000 * 1000);
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.ocompact", "-wal.compact" };
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	db_opts_t dopt = DB_OPTS_DEFAULT;
	xtc_proc_opts_t bo = { .size = sizeof(xtc_proc_opts_t), .name = "boss" };
	char side[320], sql[96];
	xtc_pid_t bp;
	pthread_t wd;
	sx_db *h = NULL;
	int fd, i;

	if (argc > 1)
		g_which = argv[1];
	if (getenv("XTC_UNDER_VALGRIND") != NULL ||
	    getenv("XTC_TEST_EMULATED") != NULL)
		g_slow = 20;
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	t_tmpl(g_path, sizeof g_path, "sqlxtc-connsup");
	if ((fd = mkstemp(g_path)) < 0)
		return 1;
	close(fd);
	unlink(g_path);
	if (sx_init() != SX_OK || sx_storage_open(g_path, 256) != SX_OK ||
	    sx_open(":memory:", &h) != SX_OK)
		return 1;
	if (sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)",
	    NULL) != SX_OK)
		return 1;
	for (i = 1; i <= 10; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, 1)", i);
		if (sx_exec(h, sql, NULL) != SX_OK)
			return 1;
	}
	sx_close(h);
	if (xtc_res_init(&g_res, &caps) != XTC_OK)
		return 1;
	dopt.res = &g_res;
	if (db_create(&dopt, &g_db) != XTC_OK)
		return 1;
	if (xtc_exec_init(&g_exec, 2) != XTC_OK ||
	    sx_storage_run(xtc_exec_loop(g_exec, 0)) != SX_OK ||
	    xtc_proc_spawn(xtc_exec_loop(g_exec, 0), boss, NULL, &bo, &bp) != XTC_OK)
		return 1;
	(void)pthread_create(&wd, NULL, watchdog, NULL);
	(void)xtc_exec_run(g_exec);
	atomic_store(&g_done, 1);
	(void)pthread_join(wd, NULL);
	(void)xtc_exec_fini(g_exec);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < (int)(sizeof sfx / sizeof sfx[0]); i++) {
		snprintf(side, sizeof side, "%s%s", g_path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_conn_sup\n");
		return 1;
	}
	printf("  ok   a killed connection releases its socket, buffers, "
	    "snapshot and row intents\n");
	return 0;
}
