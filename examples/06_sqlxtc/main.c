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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/file.h>
#include <sys/socket.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_log.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_res.h"

#include "conn.h"
#include "db.h"
#include "engine.h"
extern int metrics_spawn(xtc_loop_t *loop, xtc_res_t *res,
                         _Atomic int *conn_count,
                         _Atomic int64_t *queries_total,
                         xtc_pid_t *out_pid);
extern void metrics_stop(void);

typedef struct server_cfg {
	const char *host;
	int         port;
	const char *db_path;
	int         cores;
	int64_t     max_memory;
	int         max_clients;
	int64_t     max_iops;
	int         max_databases;
	const char *storage_path;        /* libxtc-native engine file (NULL = off) */
	unsigned int storage_frames;     /* xstore buffer-pool frames (0 = default) */
	int          storage_direct;     /* open the page store with direct I/O */
	int          storage_adaptive;   /* GA-tuned adaptive writeback */
	int         shared_handle;       /* 1 default; --no-shared turns off */
	int         threads;             /* worker loops; 0 = auto (ncpus) */
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
	.storage_path = NULL, \
	.storage_frames = 0,  \
	.shared_handle = 1,   \
	.threads = 1,         \
	.verbose = 0          \
}

typedef struct server {
	server_cfg_t  cfg;
	db_t         *db;
	xtc_res_t    *res;
	xtc_app_t    *app;            /* supervised multi-loop application */
	xtc_loop_t   *loop;            /* loop 0: listener, metrics, storage */
	xtc_exec_t   *exec;           /* N worker loops (NULL if single-loop) */
	int           n_loops;
	_Atomic unsigned next_loop;   /* round-robin cursor for conn placement */
	int           listen_fd;

	_Atomic int      conn_count;
	_Atomic int64_t  queries_total;

	_Atomic int64_t  iops_tokens;
	int64_t          iops_refill_ns;
	int64_t          iops_last_refill;

	_Atomic int      shutdown_requested;
} server_t;

static server_t g_server;

/* A storage proc that cannot be restarted exited, or one that can ran
 * out of restarts (sx_storage_on_failure; runs on a proc on loop 0).
 * Stop the server through the SIGTERM path -- the drain_on_signal
 * handler only writes its self-pipe -- and exit non-zero. */
static _Atomic int g_storage_failed;

static void
storage_failed(const char *what, void *arg)
{
	(void)arg;
	if (atomic_exchange(&g_storage_failed, 1))
		return;
	XTC_LOG_ERROR_F("sqlxtc: storage proc %s exited; stopping the server",
	    what);
	(void)kill(getpid(), SIGTERM);
}

void
server_inc_conn(server_t *s) { atomic_fetch_add(&s->conn_count, 1); }

void
server_dec_conn(server_t *s) { atomic_fetch_sub(&s->conn_count, 1); }

static void
rate_limit_init(server_t *s)
{
	if (s->cfg.max_iops > 0) {
		atomic_store(&s->iops_tokens, s->cfg.max_iops);
		s->iops_refill_ns = 1000000000LL;
		s->iops_last_refill = xtc_clock_mono();
	}
}

static void
rate_limit_refill(server_t *s)
{
	int64_t now;
	if (s->cfg.max_iops <= 0) return;
	now = xtc_clock_mono();
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

/*
 * One server per store.  A second server on the same files used to start:
 * its open found the base not marked clean, rebuilt the page file and
 * renamed a compacted log over the first server's, which kept appending
 * to the unlinked old log -- so commits it acknowledged afterwards were
 * gone at the next start (test_sqlxtc_exclusive.sh).  An exclusive,
 * non-blocking flock on <store>.lock, held for the process's life (the
 * kernel drops it when the process dies, so kill -9 leaves nothing
 * stale), refuses the second server before it touches the files.
 * Returns the held fd, or -1 (errno: EWOULDBLOCK = another server).
 */
static int
store_lock(const char *store)
{
	char p[1100];
	int fd;
	(void)snprintf(p, sizeof p, "%s.lock", store);
	if ((fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) < 0)
		return -1;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		int e = errno;
		(void)close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

/* Stop taking connections: unlisten so new connects are refused rather
 * than queued on a socket nobody will accept from. */
static void
stop_accepting(server_t *srv)
{
	if (srv->listen_fd >= 0) {
		xtc_net_close(srv->listen_fd);
		srv->listen_fd = -1;
	}
}

static void
listener_proc(void *arg)
{
	server_t *srv = arg;
	void *msg; size_t msg_len;
	xtc_tcp_opts_t nodelay = { .nodelay = 1 };

	while (!atomic_load(&srv->shutdown_requested)) {
		rate_limit_refill(srv);
		for (;;) {
			int fd = -1, arc;
			conn_opts_t opts;
			xtc_pid_t pid;

			/* xtc_net_accept: non-blocking + CLOEXEC + no-SIGPIPE, and
			 * errno mapped to XTC_E_*.  Out of descriptors (EMFILE /
			 * ENFILE -> XTC_E_RESOURCE) must not spin on the still-
			 * readable listen fd: back off 10 ms so connections close
			 * and free descriptors, then retry. */
			arc = xtc_net_accept(srv->listen_fd, &fd);
			if (arc == XTC_E_AGAIN)
				break;                    /* drained */
			if (arc == XTC_E_RESOURCE || arc == XTC_E_NOMEM) {
				(void)xtc_proc_sleep(10LL * 1000 * 1000);
				break;
			}
			if (arc != XTC_OK)
				break;                    /* transient; wait for readiness */

			if (srv->cfg.max_clients > 0 &&
			    atomic_load(&srv->conn_count) >= srv->cfg.max_clients) {
				const char *reject =
				    "{\"err\":\"max_clients\"}\n";
				(void)send(fd, reject, strlen(reject),
				    MSG_DONTWAIT | MSG_NOSIGNAL);
				xtc_net_close(fd);
				continue;
			}

			/* Disable Nagle via the portable helper (the fd is already
			 * non-blocking from xtc_net_accept). */
			(void)xtc_net_apply_tcp_opts(fd, &nodelay);

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

			/* Place the connection on a worker loop (round-robin
			 * across the executor); single-loop mode uses loop 0. */
			xtc_loop_t *cl = srv->loop;
			if (srv->exec != NULL && srv->n_loops > 1)
				cl = xtc_exec_loop(srv->exec,
				    (int)(atomic_fetch_add(&srv->next_loop, 1) %
				          (unsigned)srv->n_loops));
			/* Count it BEFORE the spawn (as rexis does): the proc
			 * runs on another loop and can exit -- and decrement --
			 * before conn_spawn returns, which drove the count
			 * below zero for a moment. */
			server_inc_conn(srv);
			if (conn_spawn(cl, &opts, &pid) != XTC_OK) {
				server_dec_conn(srv);
				xtc_net_close(fd);
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
					if (xtc_app_is_shutdown_msg(msg, msg_len))
						atomic_store(&srv->shutdown_requested, 1);
					if (msg) xtc_free(msg);
				}
			}
		}
	}

	/*
	 * Drain requested (SIGTERM / SIGINT, via xtc_app_drain_on_signal).
	 * Every loop is still running, so stop the storage procs HERE, from
	 * a proc: the checkpointer finishes any checkpoint and exits,
	 * commits already in flight finish (the WAL writer is still up), new
	 * ones wait at the checkpoint gate and are never acknowledged, then
	 * the WAL writer, trickler and provider stop.  Then return; the
	 * drain halts the loops and main() flushes and closes the store.
	 * Stopping them after the loops were halted is what used to hang
	 * shutdown for 20 s.
	 */
	XTC_LOG_INFO_F("sqlxtc draining: no new connections; stopping the "
	    "storage procs");
	stop_accepting(srv);
	metrics_stop();                  /* no log drain left on the pool */
	sx_storage_quiesce();            /* no-op if the store is not running */
	XTC_LOG_INFO_F("sqlxtc storage quiesced");
}

static void
usage(FILE *fp, const char *prog)
{
	fprintf(fp,
	    "Usage: %s [options]\n"
	    "\nsqlxtc: a networked, threaded SQL engine built on libxtc.\n\n"
	    "Options:\n"
	    "  -h, --host=ADDR       bind addr (default 0.0.0.0)\n"
	    "  -p, --port=PORT       bind port (default 15432)\n"
	    "  -d, --db=PATH         database path (default :memory:); the\n"
	    "                        xstore engine file is derived as PATH.xstore\n"
	    "  -c, --cores=N         pin to N cores\n"
	    "  -m, --max-memory=N    cap on connection buffer memory, bytes\n"
	    "                        (a result past it gets {\"err\":\"OOM\"})\n"
	    "  -n, --max-clients=N   max concurrent clients (default 1000)\n"
	    "  -i, --max-iops=N      queries/sec cap (0 unlimited)\n"
	    "  -D, --max-databases=N max ATTACHed dbs (default 16)\n"
	    "      --storage=PATH    libxtc-native durable engine file\n"
	    "                        (default: derived from --db)\n"
	    "      --storage-frames=N  xstore buffer-pool pages (0=default)\n"
	    "      --storage-direct    open the store with O_DIRECT\n"
	    "      --storage-adaptive  adaptive writeback pacing\n"
	    "      --storage-claim=N   CLOCK sweep bound: after N passed-over\n"
	    "                        frames, take the next idle one\n"
	    "                        (default 128; 0 = unbounded)\n"
	    "      --checkpoint-mb=N   compact the log once it passes\n"
	    "                        max(N MiB, 2 x its last checkpoint size)\n"
	    "                        (default 64)\n"
	    "      --no-shared       accepted for compatibility (every connection\n"
	    "                        always has its own handle and transaction)\n"
	    "  -t, --threads=N       worker loops (default 1; >1 = parallel,\n"
	    "                        connection-per-proc across N OS threads)\n"
	    "  -v, --verbose\n"
	    "      --help\n",
	    prog);
}

static int
parse_args(int argc, char **argv, server_cfg_t *cfg)
{
	enum { OPT_NO_SHARED = 1000, OPT_HELP, OPT_STORAGE, OPT_STORAGE_FRAMES,
	       OPT_STORAGE_DIRECT, OPT_STORAGE_ADAPTIVE, OPT_CKPT_MB,
	       OPT_STORAGE_CLAIM };
	static struct option lo[] = {
		{ "host", required_argument, NULL, 'h' },
		{ "port", required_argument, NULL, 'p' },
		{ "db",   required_argument, NULL, 'd' },
		{ "cores", required_argument, NULL, 'c' },
		{ "max-memory", required_argument, NULL, 'm' },
		{ "max-clients", required_argument, NULL, 'n' },
		{ "max-iops", required_argument, NULL, 'i' },
		{ "max-databases", required_argument, NULL, 'D' },
		{ "storage", required_argument, NULL, OPT_STORAGE },
		{ "storage-frames", required_argument, NULL, OPT_STORAGE_FRAMES },
		{ "storage-direct", no_argument, NULL, OPT_STORAGE_DIRECT },
		{ "storage-adaptive", no_argument, NULL, OPT_STORAGE_ADAPTIVE },
		{ "storage-claim", required_argument, NULL, OPT_STORAGE_CLAIM },
		{ "checkpoint-mb", required_argument, NULL, OPT_CKPT_MB },
		{ "no-shared", no_argument, NULL, OPT_NO_SHARED },
		{ "threads", required_argument, NULL, 't' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, OPT_HELP },
		{ NULL, 0, NULL, 0 }
	};
	int c;
	*cfg = (server_cfg_t)SERVER_CFG_DEFAULT;
	while ((c = getopt_long(argc, argv, "h:p:d:c:m:n:i:D:t:v",
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
		case 't': cfg->threads = atoi(optarg); break;
		case OPT_STORAGE: cfg->storage_path = optarg; break;
		case OPT_STORAGE_FRAMES: cfg->storage_frames = (unsigned)atoi(optarg); break;
		case OPT_STORAGE_DIRECT: cfg->storage_direct = 1; break;
		case OPT_STORAGE_ADAPTIVE: cfg->storage_adaptive = 1; break;
		case OPT_STORAGE_CLAIM:
			sx_storage_set_claim((uint32_t)strtoul(optarg, NULL, 10));
			break;
		case OPT_CKPT_MB:
			sx_storage_set_checkpoint((uint64_t)strtoull(optarg, NULL, 10) << 20);
			break;
		case 'v': cfg->verbose = 1; break;
		case OPT_NO_SHARED: cfg->shared_handle = 0; break;
		case OPT_HELP: usage(stdout, argv[0]); return 1;
		default: usage(stderr, argv[0]); return -1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	server_t *srv = &g_server;
	server_cfg_t cfg;
	xtc_log_opts_t log_opts = XTC_LOG_OPTS_DEFAULT;
	xtc_log_t *log;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	xtc_tcp_opts_t tcp_opts = XTC_TCP_OPTS_DEFAULT;
	db_opts_t db_opts = DB_OPTS_DEFAULT;
	xtc_pid_t metrics_pid;
	int i, rc, exit_rc = 0, mem_store = 0;

	/* Parsing and execution have deeper stacks than a small actor,
	 * particularly with sanitizer instrumentation. Set this before
	 * creating any fibers (the runtime default is 64 KiB). */
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) {
		fprintf(stderr, "cannot configure SQL fiber stack\n");
		return 1;
	}
	/* 1 = --help (success), -1 = bad option (usage already printed). */
	if ((rc = parse_args(argc, argv, &cfg)) != 0) return rc > 0 ? 0 : 1;
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

	/* The engine is native (no SQLite mutex/mem/pcache subsystems to
	 * configure): sx_init() is a no-op kept for symmetry. */
	(void)sx_init();

	/* libxtc-native durable storage engine.  Opened before the
	 * database so the shared connection handle registers it; the
	 * background procs (WAL writer, page provider, trickler) start
	 * once the loop exists (after xtc_app_start).
	 *
	 * The xstore engine is MANDATORY: sqlxtc is a from-scratch SQL
	 * engine over libxtc storage, not a SQLite shell, so every table
	 * lives in xstore.  When --storage is not given, derive the store
	 * path from the database path (<db>.xstore), or use an in-process
	 * store for an in-memory database, so there is always a native
	 * B-tree for the native driver. */
	static char storage_derived[1024];
	if (cfg.storage_path == NULL) {
		if (cfg.db_path != NULL && cfg.db_path[0] != '\0' &&
		    strcmp(cfg.db_path, ":memory:") != 0) {
			snprintf(storage_derived, sizeof storage_derived,
			    "%s.xstore", cfg.db_path);
		} else {
			/* in-memory db: a unique per-process store file */
			snprintf(storage_derived, sizeof storage_derived,
			    "/tmp/sqlxtc-mem-%ld.xstore", (long)getpid());
			mem_store = 1;
		}
		cfg.storage_path = storage_derived;
	}
	if (cfg.storage_path != NULL) {
		/* An in-memory database's store is private by name (pid). */
		if (!mem_store && store_lock(cfg.storage_path) < 0) {
			fprintf(stderr, "sqlxtc: cannot lock %s.lock: %s\n",
			    cfg.storage_path, errno == EWOULDBLOCK ?
			    "another sqlxtc-server is using this store" :
			    strerror(errno));
			return 1;
		}
		sx_storage_set_io(cfg.storage_direct, cfg.storage_adaptive);
		rc = sx_storage_open(cfg.storage_path, cfg.storage_frames);
		if (rc != SX_OK) {
			fprintf(stderr, "sx_storage_open(%s) failed: %d\n",
			        cfg.storage_path, rc);
			return 1;
		}
		XTC_LOG_INFO_F("xstore engine open: %s (direct=%d adaptive=%d)",
		    cfg.storage_path, cfg.storage_direct, cfg.storage_adaptive);
	}

	/* xtc_res. */
	srv->res = (xtc_res_t *)calloc(1, sizeof *srv->res);
	if (!srv->res) { fprintf(stderr, "oom\n"); return 1; }
	/* MEM_BYTES meters the connection buffers (conn.c); 0 = no cap. */
	caps.mem_bytes = cfg.max_memory > 0 ? cfg.max_memory : 0;
	xtc_res_init(srv->res, &caps);

	rate_limit_init(srv);

	/* Database. */
	db_opts.path = cfg.db_path;
	db_opts.shared = cfg.shared_handle;
	db_opts.parallel = (cfg.threads != 1);   /* per-conn handles when >1 */
	db_opts.res = srv->res;
	if (db_create(&db_opts, &srv->db) != XTC_OK) {
		fprintf(stderr, "db_create failed\n"); return 1;
	}

	/* Listener socket.  No SO_REUSEPORT: there is one listener, and with
	 * it a second server bound the same port "successfully" and the
	 * kernel split new connections between the two stores.  An exclusive
	 * bind fails loudly instead (SO_REUSEADDR, the default, still allows
	 * a restart over TIME_WAIT). */
	tcp_opts.reuseport = 0;
	/* A server restarted right after a kill can find the old listening
	 * socket not yet torn down; retry for up to 2 s before giving up. */
	for (i = 0; xtc_net_listen(XTC_NET_INET, cfg.host, cfg.port,
	    &tcp_opts, &srv->listen_fd) != XTC_OK; i++) {
		if (i == 40) {
			fprintf(stderr, "listen %s:%d failed (port in use?)\n",
			        cfg.host, cfg.port);
			return 1;
		}
		(void)xtc_sleep_ns(50LL * 1000 * 1000);
	}
	xtc_net_setnonblock(srv->listen_fd);

	signal(SIGPIPE, SIG_IGN);

	/* Supervised, optionally multi-loop application.  --threads > 1
	 * makes xtc_app own an N-loop executor (loop 0 hosts the supervised
	 * listener, metrics, and the storage procs; connections are spawned
	 * round-robin across all loops so SQL runs in parallel).  --threads
	 * <= 1 is a single supervised loop.  The root supervisor restarts
	 * the listener if it crashes. */
	{
		xtc_app_opts_t app_opts = XTC_APP_OPTS_DEFAULT;
		xtc_child_spec_t kids[1];
		app_opts.name = "sqlxtc";
		app_opts.n_loops = cfg.threads > 1 ? cfg.threads : 1;
		app_opts.sup.strategy = XTC_SUP_ONE_FOR_ONE;
		app_opts.sup.max_restarts = 10;
		app_opts.sup.period_ns = 60LL * 1000 * 1000 * 1000;
		if (xtc_app_create(&app_opts, &srv->app) != XTC_OK) {
			fprintf(stderr, "app create failed\n"); return 1;
		}
		srv->exec = xtc_app_exec(srv->app);   /* NULL when single-loop */
		srv->n_loops = srv->exec ? xtc_exec_n_loops(srv->exec) : 1;
		srv->loop = xtc_app_loop(srv->app);   /* loop 0 */
		atomic_init(&srv->next_loop, 0);

		/* L3 over-budget stall watchdog (inspired by Glommio -- Glauber
		 * Costa / ScyllaDB -- its stall detector).  A single SQL query or
		 * a structure-modification that runs too long would monopolize
		 * its worker loop and stall every other connection on it; the
		 * watchdog logs the offender (with a backtrace) if any one task
		 * exceeds 250ms, turning "a bad query hung a core" from a silent
		 * tail-latency mystery into an actionable warning.  Off unless a
		 * budget is set. */
		if (srv->exec != NULL)
			xtc_exec_set_stall_budget(srv->exec, 250LL * 1000 * 1000);
		else
			xtc_loop_set_stall_budget(srv->loop, 250LL * 1000 * 1000);

		memset(kids, 0, sizeof kids);
		kids[0].name   = "listener";
		kids[0].fn     = listener_proc;
		kids[0].arg    = srv;
		kids[0].loop   = 0;
		kids[0].policy = XTC_RESTART_PERMANENT;
		if (xtc_app_start(srv->app, kids, 1) != XTC_OK) {
			fprintf(stderr, "app start failed\n"); return 1;
		}
		/* SIGTERM / SIGINT: ask the listener to drain (it quiesces the
		 * store from a proc), then halt every loop -- on -t 1 as well,
		 * where idle connection procs would otherwise keep the loop
		 * running forever. */
		if (xtc_app_drain_on_signal(srv->app, 30LL * 1000 * 1000 * 1000,
		    2LL * 1000 * 1000 * 1000, NULL) != XTC_OK) {
			fprintf(stderr, "cannot install the shutdown handler\n");
			return 1;
		}
	}

	/* Start the storage engine's background procs (group-commit WAL
	 * writer, page provider, trickler) on loop 0. */
	if (cfg.storage_path != NULL) {
		sx_storage_on_failure(storage_failed, NULL);
		(void)sx_storage_run(srv->loop);
	}

	(void)metrics_spawn(srv->loop, srv->res, &srv->conn_count,
	                    &srv->queries_total, &metrics_pid);

	XTC_LOG_INFO_F("sqlxtc ready (loops=%d max_clients=%d max_memory=%lld)",
	               srv->n_loops, cfg.max_clients, (long long)cfg.max_memory);
	xtc_log_drain(log);

	xtc_app_run(srv->app);   /* blocks until the supervisor stops the app */

	XTC_LOG_INFO_F("sqlxtc shutting down");
	xtc_log_drain(log);
	stop_accepting(srv);
	db_destroy(srv->db);
	if (cfg.storage_path != NULL &&
	    sx_storage_close() != SX_OK) {   /* checkpoint + stop procs + close */
		fprintf(stderr, "sqlxtc: shutdown flush failed; the store was "
		    "NOT marked clean (the next start replays the log)\n");
		exit_rc = 1;
	}
	if (atomic_load(&g_storage_failed))
		exit_rc = 1;
	xtc_app_destroy(srv->app);   /* joins the supervisor + frees the exec */
	free(srv->res);
	xtc_log_destroy(log);
	(void)sx_shutdown();
	return exit_rc;
}
