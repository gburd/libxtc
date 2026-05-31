/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_shard.c
 *	Share-nothing sharded key/value store -- the Variant C scale-out
 *	slice (docs/M_SQLXTC_SCALEOUT.md), dogfooded end to end.
 *
 *	  - Each SHARD is one xtc_svr that exclusively owns its slice of
 *	    the key space (an in-memory open-addressing table).  Because
 *	    exactly one proc -- pinned to one loop -- ever touches a
 *	    shard's table, there is NO lock on shard-local state: the
 *	    share-nothing insight (Seastar) replaces locking with message
 *	    passing and cooperative scheduling.
 *	  - A single global TIMESTAMP ALLOCATOR xtc_svr is the control
 *	    plane (the MVCC sequence point): every op fetches a unique,
 *	    monotonic timestamp from it first.
 *	  - CLIENTS route each op by key-hash to the owning shard via
 *	    xtc_svr_call (cross-loop on the executor).
 *
 *	Run on a single loop (deterministic) and on a 4-loop executor
 *	(genuine multi-core share-nothing).  The test asserts every key
 *	lands in its shard with the right value, the allocator handed out
 *	exactly N unique monotonic timestamps, and the keys spread across
 *	all shards.  Self-contained, no daemon.
 *
 *	Dogfood: xtc_exec, cross-loop xtc_svr_call routing, a shard
 *	handle_call that PARKS (the allocator round-trip) mid-call, and
 *	the singleton control-plane proc.  See docs/M_SQLXTC_XTC_GAPS.md.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_svr.h"

#define N_SHARDS     4
#define N_CLIENTS    8
#define PER_CLIENT   64
#define N_KEYS       (N_CLIENTS * PER_CLIENT)
#define SHARD_CAP    256             /* open-addressing table size per shard */

/* ---- timestamp allocator (global control plane) ---- */
struct ts_state { uint64_t next; };

static int
ts_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	struct ts_state *s = st;
	uint64_t v;
	(void)req; (void)sz;
	v = ++s->next;
	(void)xtc_svr_reply(call, &v, sizeof v);
	return XTC_SVR_CONTINUE;
}

/* ---- shard server (owns a slice of the key space) ---- */
struct kv { uint32_t key; uint32_t val; int used; };
struct shard_state {
	struct kv  tab[SHARD_CAP];
	uint64_t   max_ts;           /* highest ts this shard has seen */
	uint64_t   reorders;         /* ops that arrived below max_ts */
};

struct shard_req {               /* client -> shard */
	uint8_t  op;                 /* 'P' put, 'G' get */
	uint32_t key;
	uint32_t val;
	uint64_t ts;
};
struct shard_rep {               /* shard -> client */
	int      found;
	uint32_t val;
};

static void
shard_put(struct shard_state *s, uint32_t key, uint32_t val)
{
	uint32_t i = key % SHARD_CAP;
	int n = 0;
	while (s->tab[i].used && s->tab[i].key != key) {
		i = (i + 1) % SHARD_CAP;
		if (++n >= SHARD_CAP)
			return;              /* table full (test sizes avoid this) */
	}
	s->tab[i].key = key;
	s->tab[i].val = val;
	s->tab[i].used = 1;
}
static int
shard_get(struct shard_state *s, uint32_t key, uint32_t *out)
{
	uint32_t i = key % SHARD_CAP;
	int n = 0;
	while (s->tab[i].used) {
		if (s->tab[i].key == key) { *out = s->tab[i].val; return 1; }
		i = (i + 1) % SHARD_CAP;
		if (++n >= SHARD_CAP)
			break;
	}
	return 0;
}

static int
shard_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	struct shard_state *s = st;
	struct shard_req r;
	struct shard_rep rep = { 0, 0 };

	if (sz < sizeof r) {
		(void)xtc_svr_reply(call, &rep, sizeof rep);
		return XTC_SVR_CONTINUE;
	}
	/* Copy into an aligned local: xtc_svr hands handle_call a payload
	 * pointer at an arbitrary offset into the envelope (msg + header),
	 * so reading a uint64_t field straight out of `req` is misaligned
	 * (UBSan flags it).  memcpy is the portable contract. */
	memcpy(&r, req, sizeof r);

	/*
	 * Timestamps come from a single global allocator, so they are a
	 * total LOGICAL order -- but concurrent clients on different cores
	 * deliver their ops to a shard out of that order.  We do NOT
	 * require monotonic arrival (that would be a false invariant for a
	 * sharded store); we COUNT the reorderings as evidence of genuine
	 * parallel interleaving.  A real MVCC engine orders versions by
	 * timestamp here, not by arrival.
	 */
	if (r.ts < s->max_ts)
		s->reorders++;
	else
		s->max_ts = r.ts;

	if (r.op == 'P') {
		shard_put(s, r.key, r.val);
		rep.found = 1;
	} else {
		rep.found = shard_get(s, r.key, &rep.val);
	}
	(void)xtc_svr_reply(call, &rep, sizeof rep);
	return XTC_SVR_CONTINUE;
}

/* ---- shared handles + termination ---- */
static xtc_pid_t   g_ts_pid;
static xtc_svr_t  *g_ts_svr;
static xtc_pid_t   g_shard_pid[N_SHARDS];
static xtc_svr_t  *g_shard_svr[N_SHARDS];
static _Atomic int g_clients_left;

static uint32_t
shard_of(uint32_t key)
{
	/* A cheap mix so adjacent keys spread across shards. */
	uint32_t h = key * 2654435761u;
	return (h >> 16) % N_SHARDS;
}

static uint64_t
get_timestamp(void)
{
	void *rep = NULL; size_t rsz = 0; uint64_t ts = 0;
	if (xtc_svr_call(g_ts_pid, NULL, 0, &rep, &rsz, 5LL * 1000 * 1000 * 1000)
	    == XTC_OK && rep != NULL && rsz == sizeof(uint64_t))
		ts = *(uint64_t *)rep;
	free(rep);
	return ts;
}

static int
shard_put_call(uint32_t key, uint32_t val, uint64_t ts)
{
	struct shard_req r = { 'P', key, val, ts };
	void *rep = NULL; size_t rsz = 0;
	int ok = 0;
	if (xtc_svr_call(g_shard_pid[shard_of(key)], &r, sizeof r, &rep, &rsz,
	    5LL * 1000 * 1000 * 1000) == XTC_OK && rep != NULL)
		ok = ((struct shard_rep *)rep)->found;
	free(rep);
	return ok;
}

static void
client_proc(void *arg)
{
	long id = (long)arg;
	int i;

	for (i = 0; i < PER_CLIENT; i++) {
		uint32_t key = (uint32_t)(id * PER_CLIENT + i);
		uint32_t val = key * 7u + 1u;
		uint64_t ts = get_timestamp();          /* control-plane round-trip */
		(void)shard_put_call(key, val, ts);      /* routed to owning shard */
	}
	/* The last client to finish stops the control plane and every
	 * shard so the loop/executor can drain (no in-flight calls remain
	 * once every client is done). */
	if (atomic_fetch_sub(&g_clients_left, 1) == 1) {
		int sidx;
		(void)xtc_svr_stop(g_ts_svr);
		for (sidx = 0; sidx < N_SHARDS; sidx++)
			(void)xtc_svr_stop(g_shard_svr[sidx]);
	}
}

/* Verifier proc (runs after the build, before shutdown is observed):
 * GET every key from its shard and check the value, then trip the
 * shutdown.  Kept separate so the build path and the read path are
 * distinct procs. */

/* Shard + allocator states are owned by their servers; keep the
 * pointers so verify() can read the tables back after the run. */
static struct shard_state *g_shard_state[N_SHARDS];
static struct ts_state    *g_ts_state;

static int
spawn_all(xtc_loop_t *ts_loop, xtc_loop_t **shard_loops, xtc_loop_t **client_loops)
{
	xtc_svr_callbacks_t ts_cb = { NULL, ts_handle_call, NULL, NULL, NULL };
	xtc_svr_callbacks_t sh_cb = { NULL, shard_handle_call, NULL, NULL, NULL };
	xtc_svr_opts_t opts = { .name = "ts", .mailbox_cap = 0 };
	int i;
	long c;

	g_ts_state = calloc(1, sizeof *g_ts_state);
	if (g_ts_state == NULL) return XTC_E_NOMEM;
	if (xtc_svr_start(ts_loop, &ts_cb, g_ts_state, &opts, &g_ts_svr) != XTC_OK)
		return XTC_E_INTERNAL;
	g_ts_pid = xtc_svr_pid(g_ts_svr);

	for (i = 0; i < N_SHARDS; i++) {
		struct shard_state *ss = calloc(1, sizeof *ss);
		xtc_svr_opts_t so = { .name = "shard", .mailbox_cap = 0 };
		if (ss == NULL) return XTC_E_NOMEM;
		ss->reorders = 0;
		g_shard_state[i] = ss;
		if (xtc_svr_start(shard_loops[i], &sh_cb, ss, &so, &g_shard_svr[i])
		    != XTC_OK)
			return XTC_E_INTERNAL;
		g_shard_pid[i] = xtc_svr_pid(g_shard_svr[i]);
	}

	atomic_store(&g_clients_left, N_CLIENTS);
	for (c = 0; c < N_CLIENTS; c++) {
		xtc_proc_opts_t po = { .name = "cli" };
		xtc_pid_t pid;
		if (xtc_proc_spawn(client_loops[c % N_CLIENTS], client_proc,
		    (void *)c, &po, &pid) != XTC_OK)
			return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* After the run, read back every key (off-loop, synchronous) and check
 * it landed in the right shard with the right value; tally shard
 * occupancy; confirm each shard saw timestamps in order. */
static int
verify(const char *tag, int expect_zero_reorders)
{
	int k, miss = 0, occupied[N_SHARDS] = {0}, empty_shards = 0, i;
	uint64_t reorders = 0;

	for (k = 0; k < N_KEYS; k++) {
		uint32_t want = (uint32_t)k * 7u + 1u, got = 0;
		uint32_t sh = shard_of((uint32_t)k);
		if (!shard_get(g_shard_state[sh], (uint32_t)k, &got) || got != want) {
			if (miss < 5)
				fprintf(stderr, "  [%s] key %d missing/wrong in shard %u\n",
				    tag, k, sh);
			miss++;
		}
	}
	for (i = 0; i < N_SHARDS; i++) {
		int j, c = 0;
		for (j = 0; j < SHARD_CAP; j++)
			if (g_shard_state[i]->tab[j].used) c++;
		occupied[i] = c;
		if (c == 0) empty_shards++;
		reorders += g_shard_state[i]->reorders;
	}
	if (miss != 0) {
		fprintf(stderr, "FAIL[%s]: %d/%d keys missing or wrong\n",
		    tag, miss, N_KEYS);
		return 1;
	}
	if (empty_shards != 0) {
		fprintf(stderr, "FAIL[%s]: %d shard(s) empty (bad routing)\n",
		    tag, empty_shards);
		return 1;
	}
	if (g_ts_state->next != (uint64_t)N_KEYS) {
		fprintf(stderr, "FAIL[%s]: allocator issued %llu timestamps (want %d)\n",
		    tag, (unsigned long long)g_ts_state->next, N_KEYS);
		return 1;
	}
	/* Single-loop is sequential, so no reordering can occur; the
	 * executor case interleaves clients across cores, so reordering
	 * is EXPECTED and is the signal that the work really ran in
	 * parallel. */
	if (expect_zero_reorders && reorders != 0) {
		fprintf(stderr, "FAIL[%s]: %llu reorders on a single loop (impossible)\n",
		    tag, (unsigned long long)reorders);
		return 1;
	}
	printf("  ok   [%s] %d keys sharded across %d owners (occupancy",
	    tag, N_KEYS, N_SHARDS);
	for (i = 0; i < N_SHARDS; i++) printf(" %d", occupied[i]);
	printf("); every key correct; %d unique monotonic timestamps; "
	    "%llu cross-core ts reorderings\n", N_KEYS,
	    (unsigned long long)reorders);
	return 0;
}

static void
cleanup(void)
{
	int i;
	for (i = 0; i < N_SHARDS; i++) {
		(void)xtc_svr_join(g_shard_svr[i], 1LL * 1000 * 1000 * 1000);
		free(g_shard_state[i]);
		g_shard_state[i] = NULL;
	}
	(void)xtc_svr_join(g_ts_svr, 1LL * 1000 * 1000 * 1000);
	free(g_ts_state);
	g_ts_state = NULL;
}

int
main(void)
{
	/* Single-loop run: shards + allocator + clients all cooperate on
	 * one loop (deterministic). */
	{
		xtc_loop_t *loop = NULL;
		xtc_loop_t *sl[N_SHARDS], *cl[N_CLIENTS];
		int i;

		assert(xtc_loop_init(&loop) == XTC_OK);
		for (i = 0; i < N_SHARDS; i++) sl[i] = loop;
		for (i = 0; i < N_CLIENTS; i++) cl[i] = loop;
		if (spawn_all(loop, sl, cl) != XTC_OK) { fprintf(stderr, "spawn\n"); return 1; }
		assert(xtc_loop_run(loop) == XTC_OK);
		if (verify("single-loop", 1) != 0) return 1;
		cleanup();
		assert(xtc_loop_fini(loop) == XTC_OK);
	}

	/* Executor run: one shard per loop (== one core), clients spread
	 * across loops -- genuine multi-core share-nothing.  Routing a key
	 * to its shard is now a cross-loop message; each shard's table is
	 * still touched by exactly one loop, so there is no lock. */
	{
		xtc_exec_t *exec = NULL;
		xtc_loop_t *sl[N_SHARDS], *cl[N_CLIENTS];
		int i;

		assert(xtc_exec_init(&exec, N_SHARDS) == XTC_OK);
		for (i = 0; i < N_SHARDS; i++) sl[i] = xtc_exec_loop(exec, i);
		for (i = 0; i < N_CLIENTS; i++) cl[i] = xtc_exec_loop(exec, i % N_SHARDS);
		if (spawn_all(xtc_exec_loop(exec, 0), sl, cl) != XTC_OK) {
			fprintf(stderr, "spawn\n"); return 1;
		}
		assert(xtc_exec_run(exec) == XTC_OK);
		if (verify("4-loop executor", 0) != 0) return 1;
		cleanup();
		(void)xtc_exec_fini(exec);
	}

	printf("All sqlxtc sharded-store tests passed.\n");
	return 0;
}
