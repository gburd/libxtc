/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/main.c
 *	rexis (Redis-protocol-compatible) server entry point.
 *
 *	Resource budgets enforced via xtc_res:
 *	  --cores=N        restrict the process to CPUs 0..N-1
 *	                   (sched_setaffinity).  rexis runs ONE loop on ONE
 *	                   thread; this does not add threads.
 *	  --max-memory=N   XTC_RES_MEM_BYTES cap over the keyspace AND the
 *	                   growth of every client's query/output buffers
 *	                   (above a fixed 16 KiB each); OOM error on exceed
 *	  --max-keys=N     per-database limit
 *	  --max-clients=N  concurrent connection limit; over it a client gets
 *	                   "-ERR max number of clients reached" and a close
 *	  --loops=N        run N event loops (OS threads) and spread the
 *	                   connections over them.  One loop holds at most
 *	                   65,520 clients (the per-loop proc ceiling, see
 *	                   conn.h), so 100k clients need --loops=2.
 *	  --max-fds=N      XTC_RES_FDS cap on the server's sockets (listener
 *	                   included), via xtc_res_attach_net; RLIMIT_NOFILE
 *	                   is raised to fit.  A client past it waits in the
 *	                   kernel backlog rather than being accepted+dropped.
 *	  --max-iops=N     rate-limit incoming commands per second
 *	  --max-request=N  largest command accepted (proto-max-bulk-len)
 *	  --max-output=N   per-client pending reply bytes
 *	                   (client-output-buffer-limit)
 *	  --timeout=SECS   close a connection idle that long (0 = never)
 *
 *	Once the socket is listening the server prints ONE line to stdout,
 *	"rexis: listening on HOST:PORT", and flushes it: a test harness waits
 *	for that line instead of guessing with a sleep.  Logs go to stderr.
 *
 *	SIGTERM / SIGINT: the listener stops accepting, the supervisor is
 *	stopped, the background procs are woken to exit, each connection
 *	proc finishes its current reply and exits, and main() returns 0.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_cfg.h"
#include "xtc_io.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_slab.h"

#include "cmd.h"
#include "conn.h"
#include "db.h"
#include "proto.h"

/* Forward declarations */
int expire_spawn(xtc_loop_t *loop, db_t *db, _Atomic int *stop,
                 xtc_pid_t *out_pid);
int metrics_spawn(xtc_loop_t *loop, db_t *db, xtc_res_t *res,
                  _Atomic int *conn_count, _Atomic int *stop,
                  xtc_pid_t *out_pid);

/* ----- Server configuration ----- */

typedef struct server_cfg {
	const char *host;
	int         port;
	int         cores;
	int64_t     max_memory;
	size_t      max_keys;
	int         max_clients;
	int64_t     max_iops;
	int64_t     max_net_mbps;
	int         verbose;
	const char *persist_dir;
	size_t      max_request;
	size_t      max_output;
	int64_t     timeout_s;
	int         loops;          /* event loops (OS threads) */
	int         loop_clients;   /* per-loop client cap (<= CONN_LOOP_MAX) */
	int64_t     max_fds;        /* XTC_RES_FDS cap; 0 = from max_clients */
} server_cfg_t;

#define SERVER_CFG_DEFAULT { \
	.host = "0.0.0.0", \
	.port = 6379, \
	.cores = 0, \
	.max_memory = 0, \
	.max_keys = 0, \
	.max_clients = 10000, \
	.max_iops = 0, \
	.max_net_mbps = 0, \
	.verbose = 0, \
	.persist_dir = NULL, \
	.max_request = CONN_DEFAULT_MAX_REQUEST, \
	.max_output = CONN_DEFAULT_MAX_OUTPUT, \
	.timeout_s = 0, \
	.loops = 1, \
	.loop_clients = CONN_LOOP_MAX, \
	.max_fds = 0 \
}

/* ----- Server state ----- */

typedef struct server {
	server_cfg_t     cfg;
	db_t            *db;
	xtc_res_t       *res;
	xtc_loop_t      *loop;          /* loop 0: listener + background procs */
	xtc_exec_t      *exec;          /* NULL with one loop */
	xtc_app_t       *app;
	int              listen_fd;

	/* Connection tracking */
	_Atomic int      conn_count;

	/* Rate limiting state (token bucket) */
	_Atomic int64_t  iops_tokens;
	int64_t          iops_refill_ns;
	int64_t          iops_last_refill;

	/* Shutdown */
	_Atomic int      shutdown_requested;
	xtc_tcp_opts_t   conn_tcp_opts;  /* applied to each accepted socket */
	xtc_pid_t        expire_pid;     /* woken by the listener on shutdown */
	xtc_pid_t        metrics_pid;
} server_t;

static server_t g_server;

/* ----- Rate limiter (token bucket) ----- */

static void
rate_limit_init(server_t *srv)
{
	if (srv->cfg.max_iops > 0) {
		atomic_store(&srv->iops_tokens, srv->cfg.max_iops);
		srv->iops_refill_ns = 1000000000LL;  /* 1 second */
		srv->iops_last_refill = xtc_clock_mono();
	}
}

static void
rate_limit_refill(server_t *srv)
{
	if (srv->cfg.max_iops <= 0)
		return;

	int64_t now;
	now = xtc_clock_mono();
	if (now - srv->iops_last_refill >= srv->iops_refill_ns) {
		/* Refill tokens */
		atomic_store(&srv->iops_tokens, srv->cfg.max_iops);
		srv->iops_last_refill = now;
	}
}

/* ----- CPU affinity ----- */

static int
pin_to_cores(int n_cores)
{
#ifdef __linux__
	cpu_set_t cpuset;
	int i;

	if (n_cores <= 0)
		return 0;

	CPU_ZERO(&cpuset);
	for (i = 0; i < n_cores; i++)
		CPU_SET(i, &cpuset);

	if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
		fprintf(stderr, "warning: sched_setaffinity failed: %s\n",
		        strerror(errno));
		return -1;
	}
	return 0;
#else
	(void)n_cores;
	return 0;
#endif
}

/* ----- Listener proc ----- */

/* Pause after a failed accept (fd exhaustion, ENOMEM). */
#define ACCEPT_BACKOFF_NS  (10LL * 1000 * 1000)
#define SHUTDOWN_POLL_NS   (10LL * 1000 * 1000)

static void
listener_proc(void *arg)
{
	server_t *srv = arg;
	int rc, backing_off = 0, waited;
	uint32_t revents;
	void *msg;
	size_t msg_len;

	while (!atomic_load(&srv->shutdown_requested)) {
		/* Refill rate limit tokens */
		rate_limit_refill(srv);

		/* Non-blocking accept loop */
		for (;;) {
			int fd, loop_idx;
			xtc_pid_t conn_pid;
			conn_opts_t opts;
			xtc_loop_t *cl;

			rc = xtc_net_accept(srv->listen_fd, &fd);
			if (rc == XTC_E_AGAIN)
				break;           /* backlog drained: park */
			if (rc != XTC_OK) {
				/* EMFILE/ENFILE (XTC_E_RESOURCE), ENOMEM, ...:
				 * the pending connection stays in the backlog,
				 * so the listen fd stays readable and parking on
				 * it would return at once -- the classic EMFILE
				 * busy loop (this proc ran at 100% CPU for the
				 * whole of an fd-exhaustion flood).  Sleep
				 * instead; a connection closing frees an fd. */
				if (!backing_off)
					XTC_LOG_WARN_F("accept: %s; backing off",
					    xtc_strerror(rc));
				backing_off = 1;
				(void)xtc_proc_sleep(ACCEPT_BACKOFF_NS);
				break;
			}
			backing_off = 0;

			/* Check connection limit.  conn_count is the number of
			 * LIVE connections: the conn proc decrements it on exit,
			 * so this is a concurrency cap, not a lifetime one. */
			if (srv->cfg.max_clients > 0 &&
			    atomic_load(&srv->conn_count) >= srv->cfg.max_clients) {
				conn_refuse(fd, "max number of clients reached");
				XTC_LOG_WARN_F("max_clients reached, rejecting");
				continue;
			}
			/* The least-loaded loop with room under its per-loop
			 * ceiling; none = every loop is full. */
			if ((loop_idx = conn_pick_loop()) < 0) {
				conn_refuse(fd, "max number of clients reached");
				XTC_LOG_WARN_F("every loop holds its %d clients; "
				    "rejecting (raise --loops)",
				    srv->cfg.loop_clients);
				continue;
			}

			/* Nagle off (xtc_net_accept already made the fd
			 * non-blocking and close-on-exec). */
			(void)xtc_net_apply_tcp_opts(fd, &srv->conn_tcp_opts);

			/* Spawn connection proc */
			memset(&opts, 0, sizeof(opts));
			opts.fd = fd;
			opts.db = srv->db;
			opts.res = srv->res;
			opts.server = srv;
			opts.pubsub = xtc_app_registry(srv->app);
			opts.conn_count = &srv->conn_count;
			opts.shutdown = &srv->shutdown_requested;
			opts.client_res = srv->res;
			opts.max_request = srv->cfg.max_request;
			opts.max_output = srv->cfg.max_output;
			opts.idle_timeout_ns = srv->cfg.timeout_s * 1000000000LL;
			opts.loop_idx = loop_idx;
			if (srv->cfg.max_iops > 0) {
				opts.iops_tokens = (int64_t *)&srv->iops_tokens;
				opts.iops_cap = srv->cfg.max_iops;
			}

			/* Count BEFORE the spawn: the proc decrements on exit,
			 * and it may run (and exit) before conn_spawn returns. */
			cl = srv->exec != NULL ?
			    xtc_exec_loop(srv->exec, loop_idx) : srv->loop;
			atomic_fetch_add(&srv->conn_count, 1);
			if ((rc = conn_spawn(cl, &opts, &conn_pid)) != XTC_OK) {
				atomic_fetch_sub(&srv->conn_count, 1);
				/* Out of procs or memory: say so, as Redis does
				 * at its limit, instead of a silent close. */
				conn_refuse(fd, "max number of clients reached");
				XTC_LOG_WARN_F("cannot start a connection proc "
				    "on loop %d: %s; rejecting", loop_idx,
				    xtc_strerror(rc));
			}
		}

		/* Wait for the next connection to arrive (or for shutdown
		 * via the kill flag).  Wakes exactly on listen-fd readiness;
		 * idle CPU is effectively zero. */
		revents = 0;
		(void)xtc_proc_wait_fd(srv->listen_fd, XTC_IO_READABLE,
		    100LL * 1000 * 1000,  /* 100ms timeout to re-check shutdown flag */
		    &revents);
		/* If the wakeup was a mailbox message, drain it (kept for
		 * compatibility with mailbox-driven control). */
		if (revents & XTC_WAIT_MAILBOX) {
			while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
				if (msg) xtc_free(msg);
			}
		}
	}

	/* Shutdown requested (the signal handler only sets the flag).
	 * Wake the two background procs so they see it now rather than at
	 * their next tick, and stop the supervisor so it does not restart
	 * us.  Connection procs see the flag on their next wake (<= 1s).
	 * Once every proc has returned the loop is empty and xtc_app_run
	 * returns in main(). */
	(void)xtc_send(srv->expire_pid, "x", 1);
	(void)xtc_send(srv->metrics_pid, "x", 1);
	/* Idle connection procs park with no deadline: wake them so they
	 * see the flag, and repeat (one between its flag check and its park
	 * misses a round) until they are gone, for up to 2 s.  With several
	 * loops this wait also matters because the executor stops when the
	 * supervisor does, freezing whatever is still running. */
	for (waited = 0; waited < 200 &&
	    atomic_load(&srv->conn_count) > 0; waited++) {
		conn_wake_all();
		(void)xtc_proc_sleep(SHUTDOWN_POLL_NS);
	}
	(void)xtc_app_stop(srv->app);
}

/* ----- Signal handling ----- */

static void
signal_handler(int sig)
{
	(void)sig;
	atomic_store(&g_server.shutdown_requested, 1);
}

static void
setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* Ignore SIGPIPE */
	signal(SIGPIPE, SIG_IGN);
}

/* ----- Usage ----- */

static void
usage(FILE *fp, const char *prog)
{
	fprintf(fp,
	    "Usage: %s [options]\n"
	    "\n"
	    "rexis (Redis-protocol-compatible) server with hard resource budgets.\n"
	    "\n"
	    "Options:\n"
	    "  -h, --host=ADDR       Bind address (default: 0.0.0.0)\n"
	    "  -p, --port=PORT       Bind port (default: 6379)\n"
	    "  -c, --cores=N         Restrict to CPUs 0..N-1 (0 = no pinning; rexis\n"
	    "                        is single-threaded, this adds no threads)\n"
	    "  -m, --max-memory=N    Memory cap in bytes over the keyspace plus all\n"
	    "                        client buffers (0 = unlimited)\n"
	    "  -k, --max-keys=N      Key count cap (0 = unlimited)\n"
	    "  -n, --max-clients=N   Max concurrent connections (default: 10000;\n"
	    "                        0 = all the loops hold);\n"
	    "                        over it: \"-ERR max number of clients reached\"\n"
	    "      --loops=N         Event loops (threads) to spread clients over\n"
	    "                        (default 1; one loop holds at most 65520)\n"
	    "      --loop-clients=N  Per-loop client cap (default and max 65520)\n"
	    "      --max-fds=N       Socket cap, listener included (default: fit\n"
	    "                        --max-clients); raises RLIMIT_NOFILE to fit\n"
	    "  -i, --max-iops=N      Rate limit commands/sec (0 = unlimited)\n"
	    "      --max-request=N   Largest command accepted, bytes (default 64 MiB)\n"
	    "      --max-output=N    Per-client unsent reply limit, bytes (default\n"
	    "                        128 MiB; a reply past it becomes an error)\n"
	    "      --timeout=SECS    Close a connection idle for SECS (0 = never)\n"
	    "      --persist=DIR     Enable Bitcask persistence in DIR (string SET/DEL only)\n"
	    "  -v, --verbose         Verbose logging\n"
	    "      --help            Show this help\n"
	    "\n"
	    "Supported Redis commands:\n"
	    "  String: GET, SET, DEL, EXISTS, INCR, DECR, INCRBY, DECRBY\n"
	    "  Keys:   EXPIRE, TTL, KEYS, FLUSHDB, DBSIZE\n"
	    "  List:   LPUSH, RPUSH, LPOP, RPOP, LLEN, LRANGE\n"
	    "  Hash:   HSET, HGET, HDEL, HKEYS, HVALS, HGETALL, HLEN\n"
	    "  Other:  PING, ECHO, INFO, COMMAND, QUIT, AUTH, SELECT\n"
	    "\n",
	    prog);
}

/* A non-negative decimal option value no larger than max.  atoi would
 * turn "--port=abc" into 0 and "--max-memory=1e6" into 1 without a
 * word; a budget flag that silently means something else is worse
 * than no flag. */
static int
parse_num(const char *opt, const char *s, long long max, long long *out)
{
	char *end;
	long long v;

	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v < 0 || v > max) {
		fprintf(stderr, "rexis: invalid value for --%s: '%s'\n", opt, s);
		return -1;
	}
	*out = v;
	return 0;
}

/* Returns 0 to run, 1 for --help (exit 0), -1 on a bad argument. */
static int
parse_args(int argc, char **argv, server_cfg_t *cfg)
{
	enum { OPT_HELP = 1000, OPT_MAX_REQUEST, OPT_MAX_OUTPUT, OPT_TIMEOUT,
	    OPT_LOOPS, OPT_LOOP_CLIENTS, OPT_MAX_FDS };
	static struct option longopts[] = {
		{ "host",        required_argument, NULL, 'h' },
		{ "port",        required_argument, NULL, 'p' },
		{ "cores",       required_argument, NULL, 'c' },
		{ "max-memory",  required_argument, NULL, 'm' },
		{ "max-keys",    required_argument, NULL, 'k' },
		{ "max-clients", required_argument, NULL, 'n' },
		{ "max-iops",    required_argument, NULL, 'i' },
		{ "persist",     required_argument, NULL, 'P' },
		{ "max-request", required_argument, NULL, OPT_MAX_REQUEST },
		{ "max-output",  required_argument, NULL, OPT_MAX_OUTPUT },
		{ "timeout",     required_argument, NULL, OPT_TIMEOUT },
		{ "loops",       required_argument, NULL, OPT_LOOPS },
		{ "loop-clients", required_argument, NULL, OPT_LOOP_CLIENTS },
		{ "max-fds",     required_argument, NULL, OPT_MAX_FDS },
		{ "verbose",     no_argument,       NULL, 'v' },
		{ "help",        no_argument,       NULL, OPT_HELP },
		{ NULL, 0, NULL, 0 }
	};
	long long v;
	int c, rc = 0;

	*cfg = (server_cfg_t)SERVER_CFG_DEFAULT;

	while (rc == 0 &&
	    (c = getopt_long(argc, argv, "h:p:c:m:k:n:i:P:v", longopts, NULL)) != -1) {
		switch (c) {
		case 'h':
			cfg->host = optarg;
			break;
		case 'p':
			if ((rc = parse_num("port", optarg, 65535, &v)) == 0 &&
			    v == 0) {
				fprintf(stderr, "rexis: --port must be 1..65535\n");
				rc = -1;
			}
			cfg->port = (int)v;
			break;
		case 'c':
			if ((rc = parse_num("cores", optarg, 4096, &v)) == 0)
				cfg->cores = (int)v;
			break;
		case 'm':
			if ((rc = parse_num("max-memory", optarg, INT64_MAX, &v)) == 0)
				cfg->max_memory = v;
			break;
		case 'k':
			if ((rc = parse_num("max-keys", optarg, INT64_MAX, &v)) == 0)
				cfg->max_keys = (size_t)v;
			break;
		case 'n':
			if ((rc = parse_num("max-clients", optarg, INT32_MAX, &v)) == 0)
				cfg->max_clients = (int)v;
			break;
		case 'i':
			if ((rc = parse_num("max-iops", optarg, INT64_MAX, &v)) == 0)
				cfg->max_iops = v;
			break;
		case 'P':
			cfg->persist_dir = optarg;
			break;
		case OPT_MAX_REQUEST:
			if ((rc = parse_num("max-request", optarg, INT64_MAX,
			    &v)) == 0)
				cfg->max_request = (size_t)v;
			break;
		case OPT_MAX_OUTPUT:
			if ((rc = parse_num("max-output", optarg, INT64_MAX,
			    &v)) == 0)
				cfg->max_output = (size_t)v;
			break;
		case OPT_TIMEOUT:
			if ((rc = parse_num("timeout", optarg, INT32_MAX,
			    &v)) == 0)
				cfg->timeout_s = v;
			break;
		case OPT_LOOPS:
			if ((rc = parse_num("loops", optarg, CONN_MAX_LOOPS,
			    &v)) == 0 && v == 0) {
				fprintf(stderr, "rexis: --loops must be 1..%d\n",
				    CONN_MAX_LOOPS);
				rc = -1;
			}
			cfg->loops = (int)v;
			break;
		case OPT_LOOP_CLIENTS:
			if ((rc = parse_num("loop-clients", optarg,
			    CONN_LOOP_MAX, &v)) == 0 && v == 0) {
				fprintf(stderr, "rexis: --loop-clients must be "
				    "1..%d\n", CONN_LOOP_MAX);
				rc = -1;
			}
			cfg->loop_clients = (int)v;
			break;
		case OPT_MAX_FDS:
			if ((rc = parse_num("max-fds", optarg, INT32_MAX,
			    &v)) == 0 && v < 2) {
				fprintf(stderr, "rexis: --max-fds must be >= 2 "
				    "(the listener and one client)\n");
				rc = -1;
			}
			cfg->max_fds = v;
			break;
		case 'v':
			cfg->verbose = 1;
			break;
		case OPT_HELP:
			usage(stdout, argv[0]);
			return 1;
		default:
			usage(stderr, argv[0]);
			return -1;
		}
	}
	if (rc == 0 && optind < argc) {
		fprintf(stderr, "rexis: unexpected argument '%s'\n", argv[optind]);
		usage(stderr, argv[0]);
		return -1;
	}
	/* The per-loop proc ceiling is hard (conn.h): refuse a client cap
	 * the loops cannot hold now, not at connection 65,521.
	 * (--loop-clients only lowers the per-loop cap; past it a client is
	 * refused at run time with the same error the ceiling gives.) */
	if (rc == 0 && cfg->max_clients == 0)
		cfg->max_clients = cfg->loops * cfg->loop_clients;  /* 0 = all */
	if (rc == 0 && (long long)cfg->max_clients >
	    (long long)cfg->loops * CONN_LOOP_MAX) {
		fprintf(stderr, "rexis: --max-clients=%d needs more than %d "
		    "loop%s: one loop holds at most %d clients (the per-loop "
		    "proc ceiling); use --loops=%lld\n", cfg->max_clients,
		    cfg->loops, cfg->loops == 1 ? "" : "s", CONN_LOOP_MAX,
		    ((long long)cfg->max_clients + CONN_LOOP_MAX - 1) /
		    CONN_LOOP_MAX);
		return -1;
	}
	return rc;
}

/* The fd budget: --max-fds, else what --max-clients needs (clients, the
 * listener, and the backend's own fds).  The soft RLIMIT_NOFILE is raised
 * to fit (an fd past it fails accept with EMFILE, which the listener
 * survives but which is not the configured cap). */
static int64_t
fd_budget(const server_cfg_t *cfg)
{
	int64_t want = cfg->max_fds > 0 ? cfg->max_fds :
	    (int64_t)cfg->max_clients + 64;
#if !defined(_WIN32)
	struct rlimit rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
	    rl.rlim_cur != RLIM_INFINITY && (int64_t)rl.rlim_cur < want + 64) {
		rl.rlim_cur = rl.rlim_max == RLIM_INFINITY ||
		    (int64_t)rl.rlim_max >= want + 64 ? (rlim_t)(want + 64) :
		    rl.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &rl);
	}
#endif
	return want;
}

/* ----- Main ----- */

int
main(int argc, char **argv)
{
	server_t *srv = &g_server;
	server_cfg_t cfg;
	xtc_app_opts_t app_opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[3];
	xtc_log_opts_t log_opts = XTC_LOG_OPTS_DEFAULT;
	xtc_log_t *log;
	xtc_res_caps_t res_caps = XTC_RES_CAPS_DEFAULT;
	xtc_tcp_opts_t tcp_opts = XTC_TCP_OPTS_DEFAULT;
	db_opts_t db_opts = DB_OPTS_DEFAULT;
	int rc;

	/* Parse command line: 1 = --help (success), -1 = bad argument. */
	if ((rc = parse_args(argc, argv, &cfg)) != 0)
		return rc > 0 ? 0 : 2;

	srv->cfg = cfg;
	atomic_init(&srv->conn_count, 0);
	atomic_init(&srv->iops_tokens, cfg.max_iops > 0 ? cfg.max_iops : 0);
	atomic_init(&srv->shutdown_requested, 0);

	/* Setup logging */
	log_opts.floor = cfg.verbose ? XTC_LOG_DEBUG : XTC_LOG_INFO;
	if (xtc_log_create(&log_opts, &log) != XTC_OK) {
		fprintf(stderr, "failed to create logger\n");
		return 1;
	}
	xtc_log_set_default(log);

	XTC_LOG_INFO_F("xtc-rexis starting on %s:%d", cfg.host, cfg.port);
	if (cfg.max_memory > 0)
		XTC_LOG_INFO_F("  max_memory: %lld bytes", (long long)cfg.max_memory);
	if (cfg.max_keys > 0)
		XTC_LOG_INFO_F("  max_keys: %zu", cfg.max_keys);
	if (cfg.max_clients > 0)
		XTC_LOG_INFO_F("  max_clients: %d", cfg.max_clients);
	XTC_LOG_INFO_F("  loops: %d (%d clients per loop)", cfg.loops,
	    cfg.loop_clients);
	if (cfg.max_iops > 0)
		XTC_LOG_INFO_F("  max_iops: %lld/sec", (long long)cfg.max_iops);
	if (cfg.cores > 0)
		XTC_LOG_INFO_F("  cores: %d", cfg.cores);

	/* Pin to cores */
	if (cfg.cores > 0) {
		pin_to_cores(cfg.cores);
	}

	/* Setup resource governor */
	if ((srv->res = xtc_calloc(1, sizeof(*srv->res))) == NULL) {
		fprintf(stderr, "failed to allocate resource state\n");
		return 1;
	}
	/* One XTC_RES_MEM_BYTES budget for the keyspace AND the growth of
	 * every client's query/output buffers (Redis counts client buffers
	 * in used_memory too; see conn.h for the fixed per-client base).  Before, only keyspace allocations were charged: 300 clients
	 * each holding a 990 KB partial command took a --max-memory=10MB
	 * server to 314 MB RSS while INFO said 0.5 MB.  0 = no cap (the
	 * xtc_res default of 1 GiB used to apply silently). */
	res_caps.mem_bytes = cfg.max_memory;
	/* XTC_RES_FDS meters every socket xtc_net_listen / _accept hands
	 * out (xtc_res_attach_net); at the cap a client waits in the kernel
	 * backlog.  Before, rexis never attached it, so the 65536 default
	 * fds cap bounded nothing. */
	res_caps.fds = fd_budget(&cfg);
	xtc_res_init(srv->res, &res_caps);
	(void)xtc_res_attach_net(srv->res);
	conn_census_init(cfg.loops, cfg.loop_clients);
	/* The fd -> pid wake table: the sockets plus the few fds the
	 * backend, the log and the store hold. */
	conn_wake_init((int)res_caps.fds + 1024);

	/* Initialize rate limiter */
	rate_limit_init(srv);

	/* Create database */
	db_opts.max_keys = cfg.max_keys;
	db_opts.max_mem_bytes = cfg.max_memory;
	db_opts.res = srv->res;
	db_opts.persist_dir = cfg.persist_dir;
	db_opts.shared = cfg.loops > 1;
	if (db_create(&db_opts, &srv->db) != XTC_OK) {
		fprintf(stderr, "failed to create database (a --max-memory "
		    "below the empty keyspace's bucket array?)\n");
		return 1;
	}
	if (cfg.persist_dir != NULL) {
		fprintf(stderr, "rexis: persistence enabled at %s\n",
		    cfg.persist_dir);
	}

	/* Setup listening socket.  No SO_REUSEPORT: rexis is ONE process,
	 * and with it a second server started on the same port binds
	 * "successfully" and the kernel silently splits the connections
	 * between the two -- a stale test server once answered half of a
	 * fresh test's requests that way.  An exclusive bind fails loudly. */
	tcp_opts.reuseport = 0;
	if (xtc_net_listen(XTC_NET_INET, cfg.host, cfg.port,
	                   &tcp_opts, &srv->listen_fd) != XTC_OK) {
		fprintf(stderr, "failed to bind to %s:%d\n", cfg.host, cfg.port);
		return 1;
	}
	xtc_net_setnonblock(srv->listen_fd);
	srv->conn_tcp_opts = (xtc_tcp_opts_t){ .nodelay = 1 };
	printf("rexis: listening on %s:%d\n", cfg.host, cfg.port);
	fflush(stdout);

	/* Initialize command table */
	cmd_init();

	/* Setup signals */
	setup_signals();

	/* Create app */
	app_opts.name = "xtc-rexis";
	app_opts.sup.strategy = XTC_SUP_ONE_FOR_ONE;
	app_opts.sup.max_restarts = 10;
	app_opts.sup.period_ns = 60LL * 1000 * 1000 * 1000;
	app_opts.n_loops = cfg.loops;

	if (xtc_app_create(&app_opts, &srv->app) != XTC_OK) {
		fprintf(stderr, "failed to create app\n");
		return 1;
	}
	srv->loop = xtc_app_loop(srv->app);
	srv->exec = xtc_app_exec(srv->app);   /* NULL with one loop */

	/* Setup supervised children.  xtc_sup_start requires at least
	 * one initial child; the listener is the natural choice. */
	memset(kids, 0, sizeof kids);
	kids[0].name = "listener";
	kids[0].fn = listener_proc;
	kids[0].arg = srv;
	kids[0].policy = XTC_RESTART_PERMANENT;

	if (xtc_app_start(srv->app, kids, 1) != XTC_OK) {
		fprintf(stderr, "failed to start app\n");
		return 1;
	}

	/* Background procs.  Not supervised: they take the shutdown flag
	 * and exit on it; the listener wakes them when it sees it.  Their
	 * pids are stored before xtc_app_run, i.e. before the listener runs. */
	rc = expire_spawn(srv->loop, srv->db, &srv->shutdown_requested,
	    &srv->expire_pid);
	if (rc != XTC_OK) {
		XTC_LOG_WARN_F("failed to spawn expire proc");
	}
	rc = metrics_spawn(srv->loop, srv->db, srv->res, &srv->conn_count,
	    &srv->shutdown_requested, &srv->metrics_pid);
	if (rc != XTC_OK) {
		XTC_LOG_WARN_F("failed to spawn metrics proc");
	}

	XTC_LOG_INFO_F("ready to accept connections");
	xtc_log_drain(log);

	/* Run the event loop.  Returns once every proc has exited, which
	 * the shutdown flag (SIGTERM / SIGINT) arranges -- see
	 * listener_proc. */
	rc = xtc_app_run(srv->app);
	if (rc != XTC_OK)
		XTC_LOG_ERROR_F("xtc_app_run: %s", xtc_strerror(rc));

	/* Cleanup */
	XTC_LOG_INFO_F("shutting down");
	xtc_log_drain(log);

	close(srv->listen_fd);
	db_destroy(srv->db);
	xtc_app_destroy(srv->app);
	xtc_free(srv->res);
	xtc_log_destroy(log);

	return rc == XTC_OK ? 0 : 1;
}
