/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/main.c
 *
 *	Phase 0 scaffold for kaka, a Kafka-shaped log broker on libxtc.
 *	Starts an xtc_app, binds a TCP listener under a supervisor, and
 *	accepts connections that it closes immediately.  Subsequent
 *	phases (see README.md) add the protocol codec, partition logs,
 *	persistence, backpressure, and consumer groups.
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_int.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_res.h"

#include "broker.h"
#include "metrics.h"

typedef struct broker_cfg {
	const char *host;
	int         port;
	const char *log_dir;
	int64_t     max_memory;
	int         max_clients;
} broker_cfg_t;

#define BROKER_CFG_DEFAULT { \
	.host = "0.0.0.0", \
	.port = 9092, \
	.log_dir = "/tmp/kaka", \
	.max_memory = 0, \
	.max_clients = 10000 \
}

typedef struct broker {
	broker_cfg_t      cfg;
	xtc_app_t        *app;
	xtc_loop_t       *loop;
	int               listen_fd;
	_Atomic int       conn_count;
	_Atomic int       shutdown_requested;
} broker_t;

static broker_t *g_broker;

static void
on_signal(int sig)
{
	(void)sig;
	if (g_broker != NULL)
		atomic_store(&g_broker->shutdown_requested, 1);
}

/*
 * Phase 0 listener: accept connections and close them.  Phase 1
 * replaces the close with spawning a per-connection xtc_proc that
 * speaks the kaka protocol, parking on the socket via
 * xtc_proc_wait_fd so an idle connection costs no CPU.
 */
static void
listener_proc(void *arg)
{
	broker_t *b = arg;

	while (!atomic_load(&b->shutdown_requested)) {
		struct sockaddr_in addr;
		socklen_t alen = sizeof addr;
		int fd = accept(b->listen_fd, (struct sockaddr *)&addr, &alen);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* Park until the listen fd is readable again;
				 * no busy spin. */
				uint32_t revents = 0;
				(void)xtc_proc_wait_fd(b->listen_fd,
				    XTC_IO_READABLE, 1000LL * 1000 * 1000,
				    &revents);
				continue;
			}
			if (errno == EINTR)
				continue;
			break;
		}
		/* Spawn a connection proc to service the socket; it parks
		 * on the fd via xtc_proc_wait_fd, so idle connections cost
		 * no CPU, and closes the fd on exit. */
		atomic_fetch_add(&b->conn_count, 1);
		xtc_net_setnonblock(fd);
		if (broker_spawn_conn(b->loop, fd) != 0)
			(void)close(fd);
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "\n"
	    "kaka -- a Kafka-shaped log broker on libxtc (Phase 0 scaffold)\n"
	    "\n"
	    "Options:\n"
	    "  -h, --host=ADDR       Bind address (default: 0.0.0.0)\n"
	    "  -p, --port=PORT       Bind port (default: 9092)\n"
	    "  -d, --dir=PATH        Log directory (default: /tmp/kaka)\n"
	    "  -m, --max-memory=N    Memory cap in bytes (0 = unlimited)\n"
	    "  -n, --max-clients=N   Max connections (default: 10000)\n"
	    "      --help            Show this help\n",
	    prog);
}

static int
parse_args(int argc, char **argv, broker_cfg_t *cfg)
{
	static struct option longopts[] = {
		{ "host",        required_argument, NULL, 'h' },
		{ "port",        required_argument, NULL, 'p' },
		{ "dir",         required_argument, NULL, 'd' },
		{ "max-memory",  required_argument, NULL, 'm' },
		{ "max-clients", required_argument, NULL, 'n' },
		{ "help",        no_argument,       NULL, '?' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	*cfg = (broker_cfg_t)BROKER_CFG_DEFAULT;
	while ((c = getopt_long(argc, argv, "h:p:d:m:n:", longopts, NULL))
	       != -1) {
		switch (c) {
		case 'h': cfg->host = optarg; break;
		case 'p': cfg->port = atoi(optarg); break;
		case 'd': cfg->log_dir = optarg; break;
		case 'm': cfg->max_memory = atoll(optarg); break;
		case 'n': cfg->max_clients = atoi(optarg); break;
		case '?':
		default:
			usage(argv[0]);
			return -1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	broker_t          *b;
	broker_cfg_t       cfg;
	xtc_log_t         *log = NULL;
	xtc_log_opts_t     log_opts = XTC_LOG_OPTS_DEFAULT;
	xtc_app_opts_t     app_opts = { 0 };
	xtc_child_spec_t   kids[1];
	xtc_tcp_opts_t     tcp_opts = XTC_TCP_OPTS_DEFAULT;

	if (parse_args(argc, argv, &cfg) != 0)
		return 1;

	if (__os_calloc(1, sizeof *b, (void **)&b) != XTC_OK || b == NULL) {
		fprintf(stderr, "oom\n");
		return 1;
	}
	b->cfg = cfg;
	g_broker = b;

	log_opts.sink_fd = 2;
	log_opts.floor = XTC_LOG_INFO;
	if (xtc_log_create(&log_opts, &log) == XTC_OK)
		xtc_log_set_default(log);

	tcp_opts.reuseaddr = 1;
	tcp_opts.reuseport = 1;
	if (xtc_net_listen(XTC_NET_INET, cfg.host, cfg.port,
	                   &tcp_opts, &b->listen_fd) != XTC_OK) {
		fprintf(stderr, "listen %s:%d failed\n", cfg.host, cfg.port);
		return 1;
	}
	xtc_net_setnonblock(b->listen_fd);

	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);

	app_opts.name = "kaka";
	app_opts.sup.strategy = XTC_SUP_ONE_FOR_ONE;
	app_opts.sup.max_restarts = 10;
	app_opts.sup.period_ns = 60LL * 1000 * 1000 * 1000;
	if (xtc_app_create(&app_opts, &b->app) != XTC_OK) {
		fprintf(stderr, "app create failed\n");
		return 1;
	}
	b->loop = xtc_app_loop(b->app);
	broker_set_loop(b->loop);
	broker_set_log_dir(cfg.log_dir);

	/* Observability + memory budget: stored record payload is capped
	 * at cfg.max_memory (0 = unbounded); PRODUCE past the cap is
	 * rejected so a producer flood cannot grow the broker without
	 * limit. */
	kaka_metrics_init();
	kaka_metrics_set_mem_cap(cfg.max_memory);

	memset(kids, 0, sizeof kids);
	kids[0].name = "listener";
	kids[0].fn = listener_proc;
	kids[0].arg = b;
	kids[0].policy = XTC_RESTART_PERMANENT;

	if (xtc_app_start(b->app, kids, 1) != XTC_OK) {
		fprintf(stderr, "app start failed\n");
		return 1;
	}

	XTC_LOG_INFO_F("kaka listening on %s:%d, log dir %s (persistent)",
	    cfg.host, cfg.port, cfg.log_dir);
	if (log != NULL)
		xtc_log_drain(log);

	xtc_app_run(b->app);

	XTC_LOG_INFO_F("kaka shutting down (accepted %d connections)",
	    atomic_load(&b->conn_count));
	if (log != NULL)
		xtc_log_drain(log);

	(void)close(b->listen_fd);
	xtc_app_destroy(b->app);
	if (log != NULL)
		xtc_log_destroy(log);
	__os_free(b);
	return 0;
}
