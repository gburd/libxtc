/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/main.c
 *	rexis (Redis-protocol-compatible) server entry point.
 *
 *	Resource budgets enforced via xtc_res:
 *	  --cores=N        pin to N cores via sched_setaffinity
 *	  --max-memory=N   XTC_RES_MEM_BYTES cap; OOM error on exceed
 *	  --max-keys=N     per-database limit
 *	  --max-clients=N  concurrent connection limit
 *	  --max-iops=N     rate-limit incoming commands per second
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
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
#include "xtc_int.h"

#include "cmd.h"
#include "conn.h"
#include "db.h"
#include "proto.h"

/* Local helper: xtc __os_clock_mono uses out-param style. */
static inline int64_t xtc_now_ns(void) {
	int64_t t; (void)__os_clock_mono(&t); return t;
}

/* Forward declarations */
int expire_spawn(xtc_loop_t *loop, db_t *db, xtc_pid_t *out_pid);
int metrics_spawn(xtc_loop_t *loop, db_t *db, xtc_res_t *res,
                  _Atomic int *conn_count, xtc_pid_t *out_pid);

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
	.verbose = 0 \
}

/* ----- Server state ----- */

typedef struct server {
	server_cfg_t     cfg;
	db_t            *db;
	xtc_res_t       *res;
	xtc_loop_t      *loop;
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
} server_t;

static server_t g_server;

/* ----- Rate limiter (token bucket) ----- */

static void
rate_limit_init(server_t *srv)
{
	if (srv->cfg.max_iops > 0) {
		atomic_store(&srv->iops_tokens, srv->cfg.max_iops);
		srv->iops_refill_ns = 1000000000LL;  /* 1 second */
		(void)__os_clock_mono(&srv->iops_last_refill);
	}
}

static void
rate_limit_refill(server_t *srv)
{
	if (srv->cfg.max_iops <= 0)
		return;

	int64_t now;
	(void)__os_clock_mono(&now);
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

static void
listener_proc(void *arg)
{
	server_t *srv = arg;
	void *msg;
	size_t msg_len;

	while (!atomic_load(&srv->shutdown_requested)) {
		/* Refill rate limit tokens */
		rate_limit_refill(srv);

		/* Non-blocking accept loop */
		for (;;) {
			struct sockaddr_in addr;
			socklen_t addrlen = sizeof(addr);
			int fd;
			xtc_pid_t conn_pid;
			conn_opts_t opts;

			fd = accept(srv->listen_fd, (struct sockaddr *)&addr,
			            &addrlen);
			if (fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				if (errno == EINTR)
					continue;
				break;
			}

			/* Check connection limit */
			if (srv->cfg.max_clients > 0 &&
			    atomic_load(&srv->conn_count) >= srv->cfg.max_clients) {
				close(fd);
				XTC_LOG_WARN_F("max_clients reached, rejecting");
				continue;
			}

			/* Set socket options */
			{
				int flag = 1;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
				           &flag, sizeof(flag));
				xtc_net_setnonblock(fd);
			}

			/* Spawn connection proc */
			memset(&opts, 0, sizeof(opts));
			opts.fd = fd;
			opts.db = srv->db;
			opts.res = srv->res;
			opts.server = srv;
			if (srv->cfg.max_iops > 0) {
				opts.iops_tokens = (int64_t *)&srv->iops_tokens;
				opts.iops_cap = srv->cfg.max_iops;
			}

			if (conn_spawn(srv->loop, &opts, &conn_pid) == XTC_OK) {
				atomic_fetch_add(&srv->conn_count, 1);
			} else {
				close(fd);
			}
		}

		/* Wait for the next connection to arrive (or for shutdown
		 * via the kill flag).  Wakes exactly on listen-fd readiness;
		 * idle CPU is effectively zero. */
		{
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(srv->listen_fd,
			    XTC_IO_READABLE,
			    100LL * 1000 * 1000,  /* 100ms timeout to re-check shutdown flag */
			    &revents);
			/* If the wakeup was a mailbox message, drain it (kept
			 * for compatibility with mailbox-driven control). */
			if (revents & XTC_WAIT_MAILBOX) {
				void *msg; size_t msg_len;
				while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
					if (msg) __os_free(msg);
				}
			}
		}
	}
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
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "\n"
	    "rexis (Redis-protocol-compatible) server with hard resource budgets.\n"
	    "\n"
	    "Options:\n"
	    "  -h, --host=ADDR       Bind address (default: 0.0.0.0)\n"
	    "  -p, --port=PORT       Bind port (default: 6379)\n"
	    "  -c, --cores=N         Pin to N cores (0 = no pinning)\n"
	    "  -m, --max-memory=N    Memory cap in bytes (0 = unlimited)\n"
	    "  -k, --max-keys=N      Key count cap (0 = unlimited)\n"
	    "  -n, --max-clients=N   Max connections (default: 10000)\n"
	    "  -i, --max-iops=N      Rate limit commands/sec (0 = unlimited)\n"
	    "  -v, --verbose         Verbose logging\n"
	    "  --help                Show this help\n"
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

static int
parse_args(int argc, char **argv, server_cfg_t *cfg)
{
	static struct option longopts[] = {
		{ "host",        required_argument, NULL, 'h' },
		{ "port",        required_argument, NULL, 'p' },
		{ "cores",       required_argument, NULL, 'c' },
		{ "max-memory",  required_argument, NULL, 'm' },
		{ "max-keys",    required_argument, NULL, 'k' },
		{ "max-clients", required_argument, NULL, 'n' },
		{ "max-iops",    required_argument, NULL, 'i' },
		{ "verbose",     no_argument,       NULL, 'v' },
		{ "help",        no_argument,       NULL, '?' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	*cfg = (server_cfg_t)SERVER_CFG_DEFAULT;

	while ((c = getopt_long(argc, argv, "h:p:c:m:k:n:i:v", longopts, NULL)) != -1) {
		switch (c) {
		case 'h':
			cfg->host = optarg;
			break;
		case 'p':
			cfg->port = atoi(optarg);
			break;
		case 'c':
			cfg->cores = atoi(optarg);
			break;
		case 'm':
			cfg->max_memory = atoll(optarg);
			break;
		case 'k':
			cfg->max_keys = (size_t)atoll(optarg);
			break;
		case 'n':
			cfg->max_clients = atoi(optarg);
			break;
		case 'i':
			cfg->max_iops = atoll(optarg);
			break;
		case 'v':
			cfg->verbose = 1;
			break;
		case '?':
		default:
			usage(argv[0]);
			return -1;
		}
	}

	return 0;
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
	xtc_pid_t listener_pid, expire_pid, metrics_pid;
	int rc;

	/* Parse command line */
	if (parse_args(argc, argv, &cfg) < 0)
		return 1;

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
	if (cfg.max_iops > 0)
		XTC_LOG_INFO_F("  max_iops: %lld/sec", (long long)cfg.max_iops);
	if (cfg.cores > 0)
		XTC_LOG_INFO_F("  cores: %d", cfg.cores);

	/* Pin to cores */
	if (cfg.cores > 0) {
		pin_to_cores(cfg.cores);
	}

	/* Setup resource governor */
	if (__os_calloc(1, sizeof(*srv->res), (void **)&srv->res) != XTC_OK ||
	    srv->res == NULL) {
		fprintf(stderr, "failed to allocate resource state\n");
		return 1;
	}
	if (cfg.max_memory > 0)
		res_caps.mem_bytes = cfg.max_memory;
	xtc_res_init(srv->res, &res_caps);

	/* Initialize rate limiter */
	rate_limit_init(srv);

	/* Create database */
	db_opts.max_keys = cfg.max_keys;
	db_opts.max_mem_bytes = cfg.max_memory;
	db_opts.res = srv->res;
	if (db_create(&db_opts, &srv->db) != XTC_OK) {
		fprintf(stderr, "failed to create database\n");
		return 1;
	}

	/* Setup listening socket */
	tcp_opts.reuseport = 1;
	if (xtc_net_listen(XTC_NET_INET, cfg.host, cfg.port,
	                   &tcp_opts, &srv->listen_fd) != XTC_OK) {
		fprintf(stderr, "failed to bind to %s:%d\n", cfg.host, cfg.port);
		return 1;
	}
	xtc_net_setnonblock(srv->listen_fd);

	/* Initialize command table */
	cmd_init();

	/* Setup signals */
	setup_signals();

	/* Create app */
	app_opts.name = "xtc-rexis";
	app_opts.sup.strategy = XTC_SUP_ONE_FOR_ONE;
	app_opts.sup.max_restarts = 10;
	app_opts.sup.period_ns = 60LL * 1000 * 1000 * 1000;

	if (xtc_app_create(&app_opts, &srv->app) != XTC_OK) {
		fprintf(stderr, "failed to create app\n");
		return 1;
	}
	srv->loop = xtc_app_loop(srv->app);

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

	/* Listener was spawned by the supervisor above; just record its
	 * pid as zero (we don't need it for graceful stop). */
	(void)listener_pid;

	/* Spawn expire proc */
	rc = expire_spawn(srv->loop, srv->db, &expire_pid);
	if (rc != XTC_OK) {
		XTC_LOG_WARN_F("failed to spawn expire proc");
	}

	/* Spawn metrics proc */
	rc = metrics_spawn(srv->loop, srv->db, srv->res, &srv->conn_count,
	                   &metrics_pid);
	if (rc != XTC_OK) {
		XTC_LOG_WARN_F("failed to spawn metrics proc");
	}

	XTC_LOG_INFO_F("ready to accept connections");
	xtc_log_drain(log);

	/* Run event loop */
	xtc_app_run(srv->app);

	/* Cleanup */
	XTC_LOG_INFO_F("shutting down");
	xtc_log_drain(log);

	close(srv->listen_fd);
	db_destroy(srv->db);
	xtc_app_destroy(srv->app);
	__os_free(srv->res);
	xtc_log_destroy(log);

	return 0;
}
