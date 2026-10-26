/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/conn.h
 *	Per-connection xtc_proc; reads RESP commands, dispatches to
 *	handlers, writes responses.
 */

#ifndef REXIS_CONN_H
#define REXIS_CONN_H

#include <stdatomic.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_slab.h"
#include "xtc_reg.h"

#include "db.h"

/* Forward declaration */
struct server;

/* Connection configuration */
typedef struct conn_opts {
	int             fd;
	db_t           *db;
	xtc_res_t      *res;
	xtc_slab_t     *read_slab;     /* for read buffers */
	xtc_slab_t     *write_slab;    /* for write buffers */
	struct server  *server;        /* back-pointer for stats */
	xtc_reg_t      *pubsub;        /* shared pub/sub channel registry */

	/* Rate limiting */
	int64_t        *iops_tokens;
	int64_t         iops_cap;

	/* Limits (0 = the CONN_DEFAULT_* below) */
	size_t          max_request;   /* largest command accepted, bytes */
	size_t          max_output;    /* per-client pending reply bytes */
	int64_t         idle_timeout_ns; /* close after this long without
	                                  * I/O; 0 = never (Redis `timeout`) */
	/* Budget client buffer growth (every byte above the two
	 * CONN_BASE_BUF base buffers) is charged to (XTC_RES_MEM_BYTES);
	 * NULL = uncharged.  Shared by all connections. */
	xtc_res_t      *client_res;

	/* Server-wide state the connection proc updates / watches. */
	_Atomic int    *conn_count;    /* live connections; decremented on exit */
	_Atomic int    *shutdown;      /* set by the signal handler */
	int             loop_idx;      /* the loop it runs on (census) */
} conn_opts_t;

#define CONN_DEFAULT_MAX_REQUEST  (64UL * 1024 * 1024)
#define CONN_DEFAULT_MAX_OUTPUT   (128UL * 1024 * 1024)
#define CONN_BASE_BUF             (16UL * 1024)  /* each direction */

/* Spawn a connection proc.  The proc takes ownership of the fd and
 * will close it on exit, and decrements *conn_count when it exits.
 * On failure nothing was spawned: the caller still owns the fd. */
int conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid);

/* Client buffer bytes currently charged to client_res (all
 * connections' query + output buffers above their base). */
int64_t conn_client_bytes(void);

/*
 * Per-loop connection census.  A loop holds at most 65,536 procs: a pid's
 * per-loop slot (xtc_pid_t.local_id) is 16 bits, so xtc_proc_spawn
 * refuses the 65,537th live proc on one loop with XTC_E_RESOURCE.  rexis
 * keeps CONN_LOOP_RESERVE of them for its own procs (supervisor,
 * listener, expiry, metrics) and headroom, so one loop serves at most
 * CONN_LOOP_MAX clients, and more clients need more loops (--loops).
 */
#define CONN_LOOP_PROCS    65536
#define CONN_LOOP_RESERVE  16
#define CONN_LOOP_MAX      (CONN_LOOP_PROCS - CONN_LOOP_RESERVE)
#define CONN_MAX_LOOPS     256

/* Set the loop count and the per-loop client cap (<= CONN_LOOP_MAX). */
void    conn_census_init(int n_loops, int per_loop_cap);
/* Least-loaded loop with room for one more client, or -1 if all full.
 * Listener only (not thread-safe against itself). */
int     conn_pick_loop(void);
int     conn_census_loops(void);
int     conn_census_live(int loop_idx);   /* clients on that loop */
int     conn_census_total(void);          /* clients on all loops */
void    conn_census_reject(void);         /* count a refused connection */
int64_t conn_census_rejected(void);
/* Conn proc wakes caused only by its park timer (INFO idle_wakeups). */
int64_t conn_idle_wakeups(void);
/* Value bytes sent by reference, not copied (INFO reply_bytes_by_ref). */
int64_t conn_ref_bytes(void);

/* Idle conn procs park with no deadline.  conn_wake_init(max_fd) sizes
 * the fd -> pid table (before the first connection); conn_wake_all wakes
 * every recorded conn proc (shutdown: each then sees the flag).  A proc
 * between its flag check and its park misses one call, so shutdown
 * repeats it until the procs are gone. */
void    conn_wake_init(int max_fd);
void    conn_wake_all(void);

/* Refuse a connection the way Redis does: send "-ERR <msg>" (best
 * effort, never blocking) and close it, counting it in the census. */
void    conn_refuse(int fd, const char *msg);

#endif /* REXIS_CONN_H */
