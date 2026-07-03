/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/main.c
 *	pgmock-server: a standalone mock PostgreSQL postmaster on the
 *	xtc scheduler.  Proves the M16.1a runtime seam -- postmaster +
 *	N backends as xtc_procs doing a PG-v3 "SELECT 1" round-trip
 *	with ZERO PostgreSQL source.  See docs/M16_PG_ADAPTER.md.
 *
 *	Usage:
 *	    pgmock-server [-h host] [-p port]      (TCP, default 127.0.0.1:15442)
 *	    pgmock-server -U /path/to/socket        (Unix domain socket)
 *
 *	Connect with psql or any raw v3 client, e.g.:
 *	    psql -h 127.0.0.1 -p 15442 -c 'select 1'
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_net.h"
#include "xtc_orc.h"
#include "xtc_proc.h"

#include "listener.h"

static pgmock_listener_t g_listener;
static xtc_app_t        *g_app;
static _Atomic int       g_shutdown;

static void
sig_handler(int sig)
{
	(void)sig;
	atomic_store(&g_shutdown, 1);
	if (g_app != NULL)
		(void)xtc_app_stop(g_app);
}

int
main(int argc, char **argv)
{
	const char *host = "127.0.0.1";
	const char *unix_path = NULL;
	int port = 15442;
	int listen_fd = -1;
	int c;
	xtc_app_opts_t app_opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];

	while ((c = getopt(argc, argv, "h:p:U:")) != -1) {
		switch (c) {
		case 'h': host = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'U': unix_path = optarg; break;
		default:
			fprintf(stderr,
			    "usage: %s [-h host] [-p port] [-U unix_path]\n",
			    argv[0]);
			return 1;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	{
		struct sigaction sa = { 0 };
		sa.sa_handler = sig_handler;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}

	if (unix_path != NULL) {
		unlink(unix_path);
		if (xtc_net_unix_listen(unix_path, &listen_fd) != XTC_OK) {
			fprintf(stderr, "unix listen %s failed\n", unix_path);
			return 1;
		}
		fprintf(stderr, "pgmock listening on unix:%s\n", unix_path);
	} else {
		xtc_tcp_opts_t tcp = XTC_TCP_OPTS_DEFAULT;
		if (xtc_net_listen(XTC_NET_INET, host, port, &tcp,
		                   &listen_fd) != XTC_OK) {
			fprintf(stderr, "listen %s:%d failed\n", host, port);
			return 1;
		}
		fprintf(stderr, "pgmock listening on %s:%d\n", host, port);
	}
	xtc_net_setnonblock(listen_fd);

	app_opts.name = "pgmock";
	app_opts.n_loops = 1;
	if (xtc_app_create(&app_opts, &g_app) != XTC_OK) {
		fprintf(stderr, "app create failed\n");
		return 1;
	}

	atomic_init(&g_shutdown, 0);
	atomic_init(&g_listener.accepted, 0);
	g_listener.listen_fd = listen_fd;
	g_listener.loop = xtc_app_loop(g_app);
	g_listener.shutdown = &g_shutdown;

	memset(kids, 0, sizeof kids);
	kids[0].name = "postmaster";
	kids[0].fn = pgmock_listener_proc;
	kids[0].arg = &g_listener;
	kids[0].loop = 0;
	kids[0].policy = XTC_RESTART_PERMANENT;
	if (xtc_app_start(g_app, kids, 1) != XTC_OK) {
		fprintf(stderr, "app start failed\n");
		return 1;
	}

	xtc_app_run(g_app);   /* blocks until stopped */

	xtc_app_destroy(g_app);
	xtc_net_close(listen_fd);
	if (unix_path != NULL)
		unlink(unix_path);
	fprintf(stderr, "pgmock exiting (accepted %d connections)\n",
	    atomic_load(&g_listener.accepted));
	return 0;
}
