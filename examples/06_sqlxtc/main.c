/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/main.c
 *	sqlxtc-server entry point.  Wires up xtc_app, xtc_res, the
 *	listener xtc_proc, the SQLite mutex methods, and per-connection
 *	procs.
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_int.h"
#include "xtc_io.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_res.h"

#include "conn.h"
#include "db.h"
#include "sqlite/sqlite3.h"

extern const sqlite3_mutex_methods *xtc_sqlite_mutex_methods(void);
extern int metrics_spawn(xtc_loop_t *loop, xtc_res_t *res,
                         _Atomic int *conn_count,
                         _Atomic int64_t *queries_total,
                         xtc_pid_t *out_pid);

typedef struct server_cfg {
	const char *host;
	int         port;
	const char *db_path;
	int         cores;
	int64_t     max_memory;
	int         max_clients;
	int64_t     max_iops;
	int         max_databases;
	int         shared_handle;       /* 1 default; --no-shared turns off */
	int         verbose;
} server_cfg_t;

#define SERVER_CFG_DEFAULT { \
	.host = "0.0.0.0",    \
	.port = 15432,        \
	.db_path = ":memory:",\
	.cores = 0,           \
	.max_memory = 0,      \
	.max_clients = 1000,  \
	.max_iops = 0,        \
	.max_databases = 16,  \
	.shared_handle = 1,   \
	.verbose = 0          \
}

typedef struct server {
	server_cfg_t  cfg;
	db_t         *db;
	xtc_res_t    *res;
	xtc_loop_t   *loop;
	xtc_app_t    *app;
	int           listen_fd;

	_Atomic int      conn_count;
	_Atomic int64_t  queries_total;

	_Atomic int64_t  iops_tokens;
	int64_t          iops_refill_ns;
	int64_t          iops_last_refill;

	_Atomic int      shutdown_requested;
} server_t;

static server_t g_server;

void
server_inc_conn(server_t *s) { atomic_fetch_add(&s->conn_count, 1); }

void
server_dec_conn(server_t *s) { atomic_fetch_sub(&s->conn_count, 1); }

int
server_take_iops(server_t *s, int n)
{
	if (s->cfg.max_iops <= 0) return 0;
	int64_t v = atomic_fetch_sub(&s->iops_tokens, (int64_t)n);
	return v >= n ? 0 : -1;
}

static void
rate_limit_init(server_t *s)
{
	if (s->cfg.max_iops > 0) {
		atomic_store(&s->iops_tokens, s->cfg.max_iops);
		s->iops_refill_ns = 1000000000LL;
		(void)__os_clock_mono(&s->iops_last_refill);
	}
}

static void
rate_limit_refill(server_t *s)
{
	int64_t now;
	if (s->cfg.max_iops <= 0) return;
	(void)__os_clock_mono(&now);
	if (now - s->iops_last_refill >= s->iops_refill_ns) {
		atomic_store(&s->iops_tokens, s->cfg.max_iops);
		s->iops_last_refill = now;
	}
}

static int
pin_to_cores(int n_cores)
{
#ifdef __linux__
	cpu_set_t cpuset;
	int i;
	if (n_cores <= 0) return 0;
	CPU_ZERO(&cpuset);
	for (i = 0; i < n_cores; i++) CPU_SET(i, &cpuset);
	if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
		fprintf(stderr,
		        "warning: sched_setaffinity failed: %s\n",
		        strerror(errno));
		return -1;
	}
	return 0;
#else
	(void)n_cores;
	return 0;
#endif
}

static void
listener_proc(void *arg)
{
	server_t *srv = arg;
	void *msg; size_t msg_len;

	while (!atomic_load(&srv->shutdown_requested)) {
		rate_limit_refill(srv);

		for (;;) {
			struct sockaddr_in addr;
			socklen_t alen = sizeof addr;
			int fd;
			conn_opts_t opts;
			xtc_pid_t pid;

			fd = accept(srv->listen_fd, (struct sockaddr *)&addr,
			            &alen);
			if (fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				if (errno == EINTR) continue;
				break;
			}

			if (srv->cfg.max_clients > 0 &&
			    atomic_load(&srv->conn_count) >= srv->cfg.max_clients) {
				const char *reject =
				    "{\"err\":\"max_clients\"}\n";
				(void)send(fd, reject, strlen(reject),
				    MSG_DONTWAIT | MSG_NOSIGNAL);
				close(fd);
				continue;
			}

			{
				int flag = 1;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
				           &flag, sizeof flag);
				xtc_net_setnonblock(fd);
			}

			memset(&opts, 0, sizeof opts);
			opts.fd = fd;
			opts.db = srv->db;
			opts.res = srv->res;
			opts.server = srv;
			opts.max_memory = srv->cfg.max_memory;
			if (srv->cfg.max_iops > 0) {
				opts.iops_tokens =
				    (int64_t *)&srv->iops_tokens;
				opts.iops_cap = srv->cfg.max_iops;
			}

			if (conn_spawn(srv->loop, &opts, &pid) == XTC_OK) {
				server_inc_conn(srv);
			} else {
				close(fd);
			}
		}

		/* Wait for the next connection arrival.  Wakes exactly when
		 * the listen fd is readable, so idle CPU is zero. */
		{
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(srv->listen_fd,
			    XTC_IO_READABLE,
			    100LL * 1000 * 1000,  /* 100ms cap to re-check shutdown */
			    &revents);
			if (revents & XTC_WAIT_MAILBOX) {
				while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
					if (msg) __os_free(msg);
				}
			}
		}
	}
}

static void
sig_handler(int sig)
{
	(void)sig;
	atomic_store(&g_server.shutdown_requested, 1);
}

static void
setup_signals(void)
{
	struct sigaction sa = { 0 };
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "\nsqlxtc: networked, threaded SQLite via xtc.\n\n"
	    "Options:\n"
	    "  -h, --host=ADDR       bind addr (default 0.0.0.0)\n"
	    "  -p, --port=PORT       bind port (default 15432)\n"
	    "  -d, --db=PATH         database file (default :memory:)\n"
	    "  -c, --cores=N         pin to N cores\n"
	    "  -m, --max-memory=N    xtc_res memory cap, bytes\n"
	    "  -n, --max-clients=N   max concurrent clients (default 1000)\n"
	    "  -i, --max-iops=N      queries/sec cap (0 unlimited)\n"
	    "  -D, --max-databases=N max ATTACHed dbs (default 16)\n"
	    "      --no-shared       per-conn sqlite3 (Phase 1 mode)\n"
	    "  -v, --verbose\n"
	    "      --help\n",
	    prog);
}

static int
parse_args(int argc, char **argv, server_cfg_t *cfg)
{
	enum { OPT_NO_SHARED = 1000, OPT_HELP };
	static struct option lo[] = {
		{ "host", required_argument, NULL, 'h' },
		{ "port", required_argument, NULL, 'p' },
		{ "db",   required_argument, NULL, 'd' },
		{ "cores", required_argument, NULL, 'c' },
		{ "max-memory", required_argument, NULL, 'm' },
		{ "max-clients", required_argument, NULL, 'n' },
		{ "max-iops", required_argument, NULL, 'i' },
		{ "max-databases", required_argument, NULL, 'D' },
		{ "no-shared", no_argument, NULL, OPT_NO_SHARED },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, OPT_HELP },
		{ NULL, 0, NULL, 0 }
	};
	int c;
	*cfg = (server_cfg_t)SERVER_CFG_DEFAULT;
	while ((c = getopt_long(argc, argv, "h:p:d:c:m:n:i:D:v",
	                        lo, NULL)) != -1) {
		switch (c) {
		case 'h': cfg->host = optarg; break;
		case 'p': cfg->port = atoi(optarg); break;
		case 'd': cfg->db_path = optarg; break;
		case 'c': cfg->cores = atoi(optarg); break;
		case 'm': cfg->max_memory = atoll(optarg); break;
		case 'n': cfg->max_clients = atoi(optarg); break;
		case 'i': cfg->max_iops = atoll(optarg); break;
		case 'D': cfg->max_databases = atoi(optarg); break;
		case 'v': cfg->verbose = 1; break;
		case OPT_NO_SHARED: cfg->shared_handle = 0; break;
		case OPT_HELP:
		default: usage(argv[0]); return -1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	server_t *srv = &g_server;
	server_cfg_t cfg;
	xtc_app_opts_t app_opts = XTC_APP_OPTS_DEFAULT;
	xtc_log_opts_t log_opts = XTC_LOG_OPTS_DEFAULT;
	xtc_log_t *log;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	xtc_tcp_opts_t tcp_opts = XTC_TCP_OPTS_DEFAULT;
	db_opts_t db_opts = DB_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_pid_t metrics_pid;
	int rc;

	if (parse_args(argc, argv, &cfg) < 0) return 1;
	srv->cfg = cfg;
	atomic_init(&srv->conn_count, 0);
	atomic_init(&srv->queries_total, 0);
	atomic_init(&srv->iops_tokens, cfg.max_iops > 0 ? cfg.max_iops : 0);
	atomic_init(&srv->shutdown_requested, 0);

	/* Logging. */
	log_opts.floor = cfg.verbose ? XTC_LOG_DEBUG : XTC_LOG_INFO;
	if (xtc_log_create(&log_opts, &log) != XTC_OK) {
		fprintf(stderr, "log create failed\n"); return 1;
	}
	xtc_log_set_default(log);

	XTC_LOG_INFO_F("sqlxtc starting on %s:%d db=%s",
	               cfg.host, cfg.port, cfg.db_path);
	if (cfg.cores > 0) pin_to_cores(cfg.cores);

	/* SQLite global config: install xtc_lwlock-backed mutex methods
	 * and serialized threading mode BEFORE any sqlite3_open. */
	rc = sqlite3_config(SQLITE_CONFIG_MUTEX,
	                    xtc_sqlite_mutex_methods());
	if (rc != SQLITE_OK) {
		fprintf(stderr,
		        "sqlite3_config(MUTEX) failed: %d\n", rc);
		/* Continue with default mutex; not fatal. */
	}
	rc = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
	if (rc != SQLITE_OK) {
		fprintf(stderr,
		        "sqlite3_config(SERIALIZED) failed: %d\n", rc);
	}
	(void)sqlite3_initialize();

	/* xtc_res. */
	srv->res = (xtc_res_t *)calloc(1, sizeof *srv->res);
	if (!srv->res) { fprintf(stderr, "oom\n"); return 1; }
	if (cfg.max_memory > 0) caps.mem_bytes = cfg.max_memory;
	xtc_res_init(srv->res, &caps);

	rate_limit_init(srv);

	/* Database. */
	db_opts.path = cfg.db_path;
	db_opts.shared = cfg.shared_handle;
	db_opts.res = srv->res;
	if (db_create(&db_opts, &srv->db) != XTC_OK) {
		fprintf(stderr, "db_create failed\n"); return 1;
	}

	/* Listener socket. */
	tcp_opts.reuseport = 1;
	if (xtc_net_listen(XTC_NET_INET, cfg.host, cfg.port,
	                   &tcp_opts, &srv->listen_fd) != XTC_OK) {
		fprintf(stderr, "listen %s:%d failed\n",
		        cfg.host, cfg.port);
		return 1;
	}
	xtc_net_setnonblock(srv->listen_fd);

	setup_signals();

	app_opts.name = "sqlxtc";
	app_opts.sup.strategy = XTC_SUP_ONE_FOR_ONE;
	app_opts.sup.max_restarts = 10;
	app_opts.sup.period_ns = 60LL * 1000 * 1000 * 1000;
	if (xtc_app_create(&app_opts, &srv->app) != XTC_OK) {
		fprintf(stderr, "app create failed\n"); return 1;
	}
	srv->loop = xtc_app_loop(srv->app);

	memset(kids, 0, sizeof kids);
	kids[0].name = "listener";
	kids[0].fn = listener_proc;
	kids[0].arg = srv;
	kids[0].policy = XTC_RESTART_PERMANENT;

	if (xtc_app_start(srv->app, kids, 1) != XTC_OK) {
		fprintf(stderr, "app start failed\n"); return 1;
	}

	(void)metrics_spawn(srv->loop, srv->res, &srv->conn_count,
	                    &srv->queries_total, &metrics_pid);

	XTC_LOG_INFO_F("sqlxtc ready (max_clients=%d max_memory=%lld)",
	               cfg.max_clients, (long long)cfg.max_memory);
	xtc_log_drain(log);

	xtc_app_run(srv->app);

	XTC_LOG_INFO_F("sqlxtc shutting down");
	xtc_log_drain(log);
	close(srv->listen_fd);
	db_destroy(srv->db);
	xtc_app_destroy(srv->app);
	free(srv->res);
	xtc_log_destroy(log);
	(void)sqlite3_shutdown();
	return 0;
}
