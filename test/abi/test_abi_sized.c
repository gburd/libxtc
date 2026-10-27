/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
/*
 * test/abi/test_abi_sized.c -- the size-first struct contract (2.0 ABI).
 *
 * Simulates a consumer built against an OLDER (smaller) or NEWER (larger)
 * header than the library it links: the struct is cut to a prefix, or
 * padded with a tail, and its .size says so.  The library must not read
 * or write past .size (each prefix is heap-allocated at EXACTLY its size,
 * so an AddressSanitizer build turns any overrun into a hard failure; the
 * plain build checks guard bytes and defaults), must treat fields past
 * .size as their defaults, must reject size 0, and must reject a larger
 * struct whose unknown tail is not zero.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_inspect.h"
#include "xtc_runtime.h"
#include "xtc_exec.h"
#include "xtc_orc.h"
#include "xtc_svr.h"
#include "xtc_fsm.h"
#include "xtc_launch.h"
#include "xtc_net.h"
#include "xtc_io.h"
#include "xtc_fs.h"
#include "xtc_tls.h"
#include "xtc_slab.h"
#include "xtc_res.h"
#include "xtc_log.h"
#include "xtc_cfg.h"

static int fails;

#define CHECK(c) do {                                                   \
	if (!(c)) {                                                     \
		fprintf(stderr, "  [abi] FAIL %s:%d: %s\n", __FILE__,   \
		    __LINE__, #c);                                      \
		fails++;                                                \
	}                                                               \
} while (0)

/* An N-1 consumer's xtc_proc_opts_t: it ends at mailbox_cap. */
struct old_proc_opts {
	size_t      size;
	const char *name;
	size_t      mailbox_cap;
};

/* An N-1 consumer's xtc_proc_info_t: it ends at pid. */
struct old_proc_info {
	size_t    size;
	xtc_pid_t pid;
};

/* N-1 output structs, each ending one field into the 2.0 layout. */
struct old_runtime_info { size_t size; int n_loops; };
struct old_mailbox_stats { size_t size; size_t depth; };
struct old_loop_stats { size_t size; uint64_t tasks_run; };
struct old_down_info { size_t size; xtc_pid_t pid; };

/* N-1 input structs. */
struct old_sup_opts {             /* ends at max_restarts */
	size_t                 size;
	xtc_restart_strategy_t strategy;
	int                    max_restarts;
};
struct old_child_spec {           /* ends at policy: cap/loop default */
	size_t               size;
	const char          *name;
	xtc_proc_fn          fn;
	void                *arg;
	xtc_restart_policy_t policy;
};
struct old_svr_callbacks {        /* ends at handle_info */
	size_t size;
	int  (*init)(void *);
	int  (*handle_call)(void *, const void *, size_t, xtc_svr_call_t *);
	int  (*handle_cast)(void *, const void *, size_t);
	int  (*handle_info)(void *, const void *, size_t);
};
struct old_launch_opts { size_t size; const char *name; };
struct old_tcp_opts { size_t size; int nodelay; };
struct old_slab_opts { size_t size; const char *name; size_t obj_size; };
struct old_slab_stats { size_t size; uint64_t alloc_fast; };
struct old_res_caps { size_t size; int64_t tasks; };
struct old_log_opts { size_t size; int ring_size; };
struct old_cfg_spec {             /* ends at kind: no default, no bounds */
	size_t         size;
	const char    *name;
	const char    *short_desc;
	xtc_cfg_kind_t kind;
};
struct old_fs_stat { size_t size; int64_t bytes; };
struct old_tls_opts {             /* ends at ca_file */
	size_t      size;
	const char *cert_file;
	const char *key_file;
	const char *ca_file;
};

static void *
xalloc(size_t n)
{
	void *p = malloc(n);

	if (p == NULL) {
		perror("malloc");
		exit(2);
	}
	return p;
}

/* Runs as the proc spawned from the truncated opts: checks itself. */
static void
self_check(void *arg)
{
	struct old_proc_info *oi;
	xtc_proc_info_t full = XTC_PROC_INFO_INIT;
	unsigned char *big;
	size_t i, bigsz = sizeof(xtc_proc_info_t) + 32;
	int all_zero = 1;

	(void)arg;
	/* The opts prefix set mailbox_cap = 7; everything past it defaulted. */
	CHECK(xtc_proc_info(xtc_self(), &full) == XTC_OK);
	CHECK(full.mbox_cap == 7);
	CHECK(full.size == sizeof(xtc_proc_info_t));

	/* OLDER output struct: written only up to its size. */
	oi = xalloc(sizeof *oi);
	oi->size = sizeof *oi;
	CHECK(xtc_proc_info(xtc_self(), (xtc_proc_info_t *)(void *)oi) ==
	    XTC_OK);
	CHECK(xtc_pid_eq(oi->pid, xtc_self()));
	CHECK(oi->size == sizeof *oi);
	free(oi);

	/* NEWER output struct: the bytes this library does not know are 0. */
	big = xalloc(bigsz);
	memset(big, 0xA5, bigsz);
	memcpy(big, &bigsz, sizeof bigsz);
	CHECK(xtc_proc_info(xtc_self(), (xtc_proc_info_t *)(void *)big) ==
	    XTC_OK);
	for (i = sizeof(xtc_proc_info_t); i < bigsz; i++)
		if (big[i] != 0)
			all_zero = 0;
	CHECK(all_zero);
	free(big);

	/* size 0: rejected, nothing written. */
	full.size = 0;
	full.mbox_cap = 12345;
	CHECK(xtc_proc_info(xtc_self(), &full) == XTC_E_INVAL);
	CHECK(full.mbox_cap == 12345);
}

/* ---- G2: runtime / mailbox / loop-stats / down outputs ---------------- */

static int g_g2_ran;

static void
g2_out_proc(void *arg)
{
	struct old_runtime_info *ri;
	struct old_mailbox_stats *ms;
	struct old_down_info *di;
	xtc_runtime_info_t rz = XTC_RUNTIME_INFO_INIT;
	xtc_mailbox_stats_t mz = XTC_MAILBOX_STATS_INIT;
	unsigned char down[1 + 8 + sizeof(xtc_pid_t) + 4 + 1];
	xtc_pid_t me = xtc_self();
	int reason = 0;

	(void)arg;
	ri = xalloc(sizeof *ri);
	ri->size = sizeof *ri;
	CHECK(xtc_runtime_info((xtc_runtime_info_t *)(void *)ri) == XTC_OK);
	CHECK(ri->n_loops >= 1 && ri->size == sizeof *ri);
	free(ri);
	rz.size = 0;
	CHECK(xtc_runtime_info(&rz) == XTC_E_INVAL);

	ms = xalloc(sizeof *ms);
	ms->size = sizeof *ms;
	CHECK(xtc_proc_mailbox_stats(me, (xtc_mailbox_stats_t *)(void *)ms)
	    == XTC_OK);
	free(ms);
	mz.size = 0;
	CHECK(xtc_proc_mailbox_stats(me, &mz) == XTC_E_INVAL);

	/* A hand-built DOWN ('D', ref, pid, reason, exit_kind), packed. */
	memset(down, 0, sizeof down);
	down[0] = 'D';
	memcpy(down + 1 + 8, &me, sizeof me);
	memcpy(down + 1 + 8 + sizeof me, &reason, sizeof reason);
	di = xalloc(sizeof *di);
	di->size = sizeof *di;
	CHECK(xtc_down_decode_ex(down, sizeof down,
	    (xtc_down_info_t *)(void *)di) == XTC_OK);
	CHECK(xtc_pid_eq(di->pid, me));
	free(di);
	g_g2_ran = 1;
}

/* ---- G2: sup opts + child spec array at an older stride ---------------- */

static _Atomic int g_kid_runs;

static void
kid(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_kid_runs, 1);
}

/* ---- G2: svr callbacks without the fields after handle_info ------------ */

static int g_init_ran;

static int
s_init(void *st)
{
	(void)st;
	g_init_ran = 1;
	return XTC_OK;
}

static void
g2_inputs(void)
{
	xtc_loop_t *loop = NULL;
	xtc_exec_t *ex = NULL;
	struct old_sup_opts *so;
	struct old_child_spec *kids;
	struct old_svr_callbacks *cb;
	struct old_loop_stats *ls;
	xtc_loop_stats_t lz = XTC_LOOP_STATS_INIT;
	xtc_supervisor_t *sup = NULL;
	xtc_svr_t *svr = NULL;
	xtc_sup_opts_t zs = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t zk[2];
	int i;

	CHECK(xtc_loop_init(&loop) == XTC_OK);

	/* Older child-spec array: 3 elements at a 32-byte stride. */
	so = xalloc(sizeof *so);
	so->size = sizeof *so;
	so->strategy = XTC_SUP_ONE_FOR_ONE;
	so->max_restarts = 3;
	kids = xalloc(3 * sizeof *kids);
	for (i = 0; i < 3; i++) {
		kids[i].size = sizeof kids[i];
		kids[i].name = "k";
		kids[i].fn = kid;
		kids[i].arg = NULL;
		kids[i].policy = XTC_RESTART_TEMPORARY;
	}
	CHECK(xtc_sup_start(loop, (const xtc_sup_opts_t *)(void *)so,
	    (const xtc_child_spec_t *)(void *)kids, 3, &sup) == XTC_OK);
	free(so);
	free(kids);
	if (sup != NULL) {
		CHECK(xtc_sup_n_children(sup) == 3);
		CHECK(xtc_sup_stop(sup) == XTC_OK);
	}
	CHECK(xtc_loop_run(loop) == XTC_OK);
	if (sup != NULL)
		CHECK(xtc_sup_join(sup, -1) == XTC_OK);
	CHECK(atomic_load(&g_kid_runs) >= 3);

	/* Mixed element sizes and size 0 are refused. */
	memset(zk, 0, sizeof zk);
	zk[0].size = sizeof zk[0];
	zk[0].fn = kid;
	zk[1].size = sizeof zk[1] - 8;
	zk[1].fn = kid;
	CHECK(xtc_sup_start(loop, NULL, zk, 2, &sup) == XTC_E_INVAL);
	zk[0].size = 0;
	CHECK(xtc_sup_start(loop, NULL, zk, 1, &sup) == XTC_E_INVAL);
	zs.size = 0;
	CHECK(xtc_sup_start(loop, &zs, NULL, 0, &sup) == XTC_E_INVAL);

	/* svr callbacks ending at handle_info: terminate/handle_continue are
	 * past the caller's object and must read as NULL, not as garbage. */
	cb = xalloc(sizeof *cb);
	memset(cb, 0, sizeof *cb);
	cb->size = sizeof *cb;
	cb->init = s_init;
	CHECK(xtc_svr_start(loop, (const xtc_svr_callbacks_t *)(void *)cb,
	    NULL, NULL, &svr) == XTC_OK);
	free(cb);
	if (svr != NULL)
		CHECK(xtc_svr_stop(svr) == XTC_OK);
	CHECK(xtc_loop_run(loop) == XTC_OK);
	if (svr != NULL)
		CHECK(xtc_svr_join(svr, -1) == XTC_OK);
	CHECK(g_init_ran);
	CHECK(xtc_loop_fini(loop) == XTC_OK);

	/* loop stats, older view */
	CHECK(xtc_exec_init(&ex, 1) == XTC_OK);
	if (ex != NULL) {
		ls = xalloc(sizeof *ls);
		ls->size = sizeof *ls;
		CHECK(xtc_exec_loop_stats(ex, 0,
		    (xtc_loop_stats_t *)(void *)ls) == XTC_OK);
		free(ls);
		lz.size = 0;
		CHECK(xtc_exec_loop_stats(ex, 0, &lz) == XTC_E_INVAL);
		CHECK(xtc_exec_fini(ex) == XTC_OK);
	}
}

static int
spawn_with(xtc_loop_t *loop, const void *opts)
{
	return xtc_proc_spawn(loop, self_check, NULL,
	    (const xtc_proc_opts_t *)opts, NULL);
}

/* ---- G3: tcp opts, fs stat, aio, tls opts --------------------------- */

static void
g3_io(void)
{
	struct old_tcp_opts *to;
	struct old_fs_stat *fo;
	struct old_tls_opts *lo;
	xtc_tcp_opts_t tz = XTC_TCP_OPTS_DEFAULT;
	xtc_fs_stat_t fz = XTC_FS_STAT_INIT;
	xtc_aio_t az = XTC_AIO_INIT;
	xtc_io_t *io = NULL;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_opts_t lz = XTC_TLS_OPTS_INIT;
	int fd, rc;

	fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	CHECK(fd >= 0);
	to = xalloc(sizeof *to);
	to->size = sizeof *to;
	to->nodelay = 1;
	CHECK(xtc_net_apply_tcp_opts(fd, (const xtc_tcp_opts_t *)(void *)to)
	    == XTC_OK);
	free(to);
	tz.size = 0;
	CHECK(xtc_net_apply_tcp_opts(fd, &tz) == XTC_E_INVAL);
	(void)close(fd);

	fo = xalloc(sizeof *fo);
	fo->size = sizeof *fo;
	CHECK(xtc_fs_stat("/", (xtc_fs_stat_t *)(void *)fo) == XTC_OK);
	free(fo);
	CHECK(xtc_fs_stat("/", &fz) == XTC_OK && fz.is_dir == 1);
	fz.size = 0;
	CHECK(xtc_fs_stat("/", &fz) == XTC_E_INVAL);

	/* An aio struct is held in place for its whole flight: a size short of
	 * the 2.0 layout is refused before anything is submitted. */
	CHECK(xtc_io_init(&io) == XTC_OK);
	if (io != NULL) {
		az.fd = 0;
		az.op = XTC_AIO_FSYNC;
		az.size = offsetof(xtc_aio_t, res);
		CHECK(xtc_io_aio_submit(io, &az) == XTC_E_INVAL);
		az.size = 0;
		CHECK(xtc_io_aio_submit(io, &az) == XTC_E_INVAL);
		CHECK(xtc_io_fini(io) == XTC_OK);
	}

	/* TLS opts cut after ca_file: verify mode, ciphers, CRLs and the
	 * passphrase callback lie past it and must read as defaults.  A
	 * client ctx with no cert needs no files. */
	lo = xalloc(sizeof *lo);
	memset(lo, 0, sizeof *lo);
	lo->size = sizeof *lo;
	rc = xtc_tls_ctx_create(XTC_TLS_CLIENT,
	    (const xtc_tls_opts_t *)(void *)lo, &ctx);
	CHECK(rc == XTC_OK || rc == XTC_E_NOSYS);
	free(lo);
	if (ctx != NULL)
		xtc_tls_ctx_destroy(ctx);
	lz.size = 0;
	CHECK(xtc_tls_ctx_create(XTC_TLS_CLIENT, &lz, &ctx) == XTC_E_INVAL);
}

/* ---- G4: slab opts/stats, res caps, log opts, cfg spec --------------- */

static void
g4_ptc(void)
{
	struct old_slab_opts *so;
	struct old_slab_stats *ss;
	struct old_res_caps *rc;
	struct old_log_opts *lo;
	struct old_cfg_spec *cs;
	xtc_slab_opts_t sz = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_t *slab = NULL;
	xtc_res_t res;
	xtc_log_t *log = NULL;
	int64_t v = -1;
	void *obj;

	/* slab: required name+obj_size only; align/chunk/magazine default. */
	so = xalloc(sizeof *so);
	so->size = sizeof *so;
	so->name = "abi";
	so->obj_size = 40;
	CHECK(xtc_slab_create((const xtc_slab_opts_t *)(void *)so, &slab) ==
	    XTC_OK);
	free(so);
	if (slab != NULL) {
		obj = xtc_slab_alloc(slab);
		CHECK(obj != NULL);
		ss = xalloc(sizeof *ss);
		ss->size = sizeof *ss;
		CHECK(xtc_slab_stat(slab, (xtc_slab_stats_t *)(void *)ss) ==
		    XTC_OK);
		free(ss);
		xtc_slab_free(slab, obj);
		xtc_slab_destroy(slab);
	}
	/* A size that stops inside the REQUIRED prefix is refused. */
	sz.obj_size = 40;
	sz.size = offsetof(xtc_slab_opts_t, obj_size);
	CHECK(xtc_slab_create(&sz, &slab) == XTC_E_INVAL);

	/* res caps: only `tasks` set; channels etc. take their defaults. */
	rc = xalloc(sizeof *rc);
	rc->size = sizeof *rc;
	rc->tasks = 7;
	CHECK(xtc_res_init(&res, (const xtc_res_caps_t *)(void *)rc) ==
	    XTC_OK);
	free(rc);
	CHECK(res.caps.tasks == 7 && res.caps.channels == 4096);

	lo = xalloc(sizeof *lo);
	lo->size = sizeof *lo;
	lo->ring_size = 64;
	CHECK(xtc_log_create((const xtc_log_opts_t *)(void *)lo, &log) ==
	    XTC_OK);
	free(lo);
	if (log != NULL)
		xtc_log_destroy(log);

	/* cfg spec cut after kind: the default (an INT64 0) and the bounds
	 * past it read as 0 -- never as the bytes after the caller's object. */
	cs = xalloc(sizeof *cs);
	cs->size = sizeof *cs;
	cs->name = "abi.n1";
	cs->short_desc = NULL;
	cs->kind = XTC_CFG_INT64;
	CHECK(xtc_cfg_register((const xtc_cfg_spec_t *)(void *)cs) == XTC_OK);
	free(cs);
	CHECK(xtc_cfg_get_int64("abi.n1", &v) == XTC_OK && v == 0);
	(void)xtc_cfg_unregister("abi.n1");
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	struct old_proc_opts *oo;
	unsigned char *big;
	size_t bigsz = sizeof(xtc_proc_opts_t) + 16;
	xtc_proc_opts_t zero = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;

	CHECK(xtc_loop_init(&loop) == XTC_OK);
	if (loop == NULL)
		return 1;

	/* OLDER input struct, allocated at exactly its size. */
	oo = xalloc(sizeof *oo);
	oo->size = sizeof *oo;
	oo->name = "n-1";
	oo->mailbox_cap = 7;
	CHECK(spawn_with(loop, oo) == XTC_OK);
	free(oo);
	CHECK(xtc_loop_run(loop) == XTC_OK);

	/* size 0 / below the size field: rejected. */
	zero.size = 0;
	CHECK(xtc_proc_spawn(loop, self_check, NULL, &zero, &pid) ==
	    XTC_E_INVAL);
	zero.size = sizeof(size_t) - 1;
	CHECK(xtc_proc_spawn(loop, self_check, NULL, &zero, &pid) ==
	    XTC_E_INVAL);

	/* NEWER input struct: an unknown nonzero tail is refused ... */
	big = xalloc(bigsz);
	memset(big, 0, bigsz);
	memcpy(big, &bigsz, sizeof bigsz);
	big[bigsz - 1] = 1;
	CHECK(spawn_with(loop, big) == XTC_E_VERSION);
	/* ... and an all-zero unknown tail is accepted (those fields default).
	 * mailbox_cap is set through the library's own view so the spawned
	 * proc's mbox_cap == 7 check holds. */
	big[bigsz - 1] = 0;
	((xtc_proc_opts_t *)(void *)big)->mailbox_cap = 7;
	CHECK(spawn_with(loop, big) == XTC_OK);
	free(big);
	CHECK(xtc_loop_run(loop) == XTC_OK);

	CHECK(xtc_proc_spawn(loop, g2_out_proc, NULL, NULL, NULL) == XTC_OK);
	CHECK(xtc_loop_run(loop) == XTC_OK);
	CHECK(g_g2_ran);
	CHECK(xtc_loop_fini(loop) == XTC_OK);

	g2_inputs();
	g3_io();
	g4_ptc();
	if (fails != 0) {
		fprintf(stderr, "  [abi] FAIL: %d size-contract check(s)\n",
		    fails);
		return 1;
	}
	printf("  [abi] OK: sized structs honor .size (older, newer, 0)\n");
	return 0;
}
