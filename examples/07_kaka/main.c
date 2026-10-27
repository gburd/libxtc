/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/main.c
 *
 *	kaka, a Kafka-shaped log broker on libxtc.  Starts an xtc_app,
 *	binds a TCP listener under a supervisor, and serves the kaka
 *	protocol (PROTOCOL.md) with one proc per connection and one per
 *	partition.  SIGTERM/SIGINT drain the broker and exit.
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
#include "xtc_io.h"
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
	int64_t     fsync_interval_ms;
	int64_t     segment_bytes;
	int64_t     retention_bytes;
} broker_cfg_t;

#define BROKER_CFG_DEFAULT { \
	.host = "0.0.0.0", \
	.port = 9092, \
	.log_dir = "/tmp/kaka", \
	.max_memory = 0, \
	.max_clients = 10000, \
	.fsync_interval_ms = 0, \
	.segment_bytes = 0, \
	.retention_bytes = 0 \
}

typedef struct broker {
	broker_cfg_t      cfg;
	xtc_app_t        *app;
	xtc_loop_t       *loop;
	int               listen_fd;
	_Atomic int       conn_count;
	_Atomic int       shutdown_requested;
} broker_t;

#define MAX_SYNC_WORKERS 64     /* concurrent partition fdatasyncs */

static broker_t *g_broker;

static void
on_signal(int sig)
{
	(void)sig;
	if (g_broker == NULL)
		return;
	if (atomic_exchange(&g_broker->shutdown_requested, 1) != 0)
		_exit(1);                 /* second signal: give up now */
	(void)alarm(30);                  /* backstop: exit even if wedged */
}

/*
 * Listener: accept with xtc_net_accept and hand each socket to a
 * connection proc.  Out of descriptors (EMFILE/ENFILE) or memory, it
 * backs off 10 ms and retries, leaving the connection in the backlog,
 * instead of spinning or exiting.  It re-checks the shutdown flag at
 * least every 100 ms; on shutdown it stops accepting, drains the broker
 * (broker_shutdown) and stops the app.
 */
static void
listener_proc(void *arg)
{
	broker_t *b = arg;
	int64_t last_warn = 0;

	while (!atomic_load(&b->shutdown_requested)) {
		int fd = -1;
		int rc = xtc_net_accept(b->listen_fd, &fd);
		if (rc == XTC_OK) {
			atomic_fetch_add(&b->conn_count, 1);
			if (broker_spawn_conn(b->loop, fd) != 0)
				xtc_net_close(fd);    /* table full */
			continue;
		}
		if (rc == XTC_E_AGAIN) {
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(b->listen_fd, XTC_IO_READABLE,
			    100LL * 1000 * 1000, &revents);
			continue;
		}
		if (rc == XTC_E_INVAL) {
			XTC_LOG_ERROR_F("kaka: listen socket unusable; "
			    "shutting down");
			break;
		}
		/* XTC_E_RESOURCE (fd limit), XTC_E_NOMEM, XTC_E_IO. */
		if (xtc_clock_mono() - last_warn > 1000LL * 1000 * 1000) {
			XTC_LOG_WARN_F("kaka: accept: %s; backing off",
			    xtc_strerror(rc));
			last_warn = xtc_clock_mono();
		}
		(void)xtc_proc_sleep(10LL * 1000 * 1000);
	}
	XTC_LOG_INFO_F("kaka: shutdown requested; draining");
	xtc_net_close(b->listen_fd);
	b->listen_fd = -1;
	broker_shutdown(5LL * 1000 * 1000 * 1000);
	(void)xtc_app_stop(b->app);
}

static void
usage(FILE *fp, const char *prog)
{
	fprintf(fp,
	    "Usage: %s [options]\n"
	    "\n"
	    "kaka -- a Kafka-shaped log broker on libxtc\n"
	    "\n"
	    "Options:\n"
	    "  -h, --host=ADDR       Bind address (default: 0.0.0.0)\n"
	    "  -p, --port=PORT       Bind port (default: 9092)\n"
	    "  -d, --dir=PATH        Log directory (default: /tmp/kaka)\n"
	    "  -m, --max-memory=N    Memory cap in bytes (0 = unlimited)\n"
	    "  -n, --max-clients=N   Max connections (default: 10000)\n"
	    "      --fsync-interval-ms=N\n"
	    "                        0 (default): ack a PRODUCE only after\n"
	    "                        fdatasync (group commit).  N > 0: ack\n"
	    "                        after write(2), fdatasync at most every\n"
	    "                        N ms -- an OS crash or power loss can\n"
	    "                        lose up to N ms of acked records\n"
	    "      --segment-bytes=N Segment roll size (default: 1 MiB)\n"
	    "      --retention-bytes=N\n"
	    "                        Delete the oldest segments of a partition\n"
	    "                        beyond N bytes (default: 0 = keep all)\n"
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
		{ "fsync-interval-ms", required_argument, NULL, 'F' },
		{ "segment-bytes", required_argument, NULL, 'S' },
		{ "retention-bytes", required_argument, NULL, 'R' },
		{ "help",        no_argument,       NULL, 'H' },
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
		case 'F': cfg->fsync_interval_ms = atoll(optarg); break;
		case 'S': cfg->segment_bytes = atoll(optarg); break;
		case 'R': cfg->retention_bytes = atoll(optarg); break;
		case 'H':
			usage(stdout, argv[0]);
			return 1;
		default:
			usage(stderr, argv[0]);
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
	xtc_app_opts_t     app_opts = XTC_APP_OPTS_DEFAULT;
	xtc_sup_opts_t     app_opts_sup = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t   kids[1];
	xtc_tcp_opts_t     tcp_opts = XTC_TCP_OPTS_DEFAULT;
	int                rc;

	/* 1 = --help (success), -1 = bad option (usage already printed). */
	if ((rc = parse_args(argc, argv, &cfg)) != 0)
		return rc > 0 ? 0 : 1;

	if ((b = xtc_calloc(1, sizeof *b)) == NULL) {
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

	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);

	/* One loop runs every partition, and each fdatasync is an io_uring
	 * job on that loop's io-wq.  The library caps a ring's bounded
	 * io-wq at 4 workers (built for many-carrier hosts), which would
	 * cap the broker at 4 concurrent partition syncs. */
	xtc_io_set_iowq_max_workers(MAX_SYNC_WORKERS, 0);

	app_opts.name = "kaka";
	app_opts_sup.strategy = XTC_SUP_ONE_FOR_ONE;
	app_opts_sup.max_restarts = 10;
	app_opts_sup.period_ns = 60LL * 1000 * 1000 * 1000;
	app_opts.sup = &app_opts_sup;
	if (xtc_app_create(&app_opts, &b->app) != XTC_OK) {
		fprintf(stderr, "app create failed\n");
		return 1;
	}
	b->loop = xtc_app_loop(b->app);
	broker_set_loop(b->loop);
	broker_set_log_dir(cfg.log_dir);
	broker_set_fsync_interval(cfg.fsync_interval_ms * 1000 * 1000);
	broker_set_segment_bytes(cfg.segment_bytes > 0 ?
	    (size_t)cfg.segment_bytes : 0);
	broker_set_retention_bytes(cfg.retention_bytes > 0 ?
	    (uint64_t)cfg.retention_bytes : 0);
	if (broker_start(b->loop, cfg.max_clients) != 0) {
		fprintf(stderr, "broker start failed\n");
		return 1;
	}

	/* Observability + memory budget: stored record payload is capped
	 * at cfg.max_memory (0 = unbounded); PRODUCE past the cap is
	 * rejected so a producer flood cannot grow the broker without
	 * limit. */
	kaka_metrics_init();
	kaka_metrics_set_mem_cap(cfg.max_memory);

	memset(kids, 0, sizeof kids);
	kids[0].size = sizeof kids[0];
	kids[0].name = "listener";
	kids[0].fn = listener_proc;
	kids[0].arg = b;
	kids[0].policy = XTC_RESTART_TRANSIENT;

	if (xtc_app_start(b->app, kids, 1) != XTC_OK) {
		fprintf(stderr, "app start failed\n");
		return 1;
	}

	XTC_LOG_INFO_F("kaka listening on %s:%d, log dir %s (%s)",
	    cfg.host, cfg.port, cfg.log_dir, cfg.fsync_interval_ms > 0 ?
	    "relaxed fsync" : "fsync before ack");
	if (log != NULL)
		xtc_log_drain(log);

	xtc_app_run(b->app);

	XTC_LOG_INFO_F("kaka shutting down (accepted %d connections)",
	    atomic_load(&b->conn_count));
	if (log != NULL)
		xtc_log_drain(log);

	if (b->listen_fd >= 0)
		xtc_net_close(b->listen_fd);
	xtc_app_destroy(b->app);
	if (log != NULL)
		xtc_log_destroy(log);
	xtc_free(b);
	return 0;
}
