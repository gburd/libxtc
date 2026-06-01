/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mvcc.c
 *	Snapshot-isolation MVCC + 2PC over the share-nothing shards.
 *	See mvcc.h and docs/M_SQLXTC_STAGE4.md.
 *
 *	Shards and the coordinator are xtc_svr processes.  The
 *	coordinator's commit path is the canonical user of the
 *	gen_server deferred reply (xtc_svr_call_save): it parks the
 *	client's commit call, fans PREPARE out to the participant shards,
 *	collects their votes as they arrive, and answers the client only
 *	once every vote is in.  Each shard advances its own hybrid
 *	logical clock; no central timestamp allocator.
 */

#include "mvcc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xtc.h"
#include "xtc_svr.h"

/* ---- hybrid logical clock (per shard / per coordinator) ---- */
/* High 48 bits: physical microseconds.  Low 16 bits: logical counter.
 * Each clock is owned by one proc, so no atomics are needed. */
static uint64_t
hlc_phys_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t
hlc_tick(uint64_t *clk)
{
	uint64_t prev = *clk, pt = hlc_phys_us();
	uint64_t pphys = prev >> 16, plog = prev & 0xFFFF, next;

	if (pt > pphys)
		next = pt << 16;
	else if (plog + 1 > 0xFFFF)
		next = (pphys + 1) << 16;
	else
		next = (pphys << 16) | (plog + 1);
	*clk = next;
	return next;
}

static void
hlc_observe(uint64_t *clk, uint64_t m)
{
	/* Advance our clock past a stamp we have seen (commit_ts). */
	uint64_t prev = *clk, pt = hlc_phys_us();
	uint64_t pphys = prev >> 16, plog = prev & 0xFFFF;
	uint64_t mphys = m >> 16, mlog = m & 0xFFFF;
	uint64_t nphys = pphys, nlog;

	if (mphys > nphys) nphys = mphys;
	if (pt > nphys) nphys = pt;
	if (nphys == pphys && nphys == mphys) nlog = (plog > mlog ? plog : mlog) + 1;
	else if (nphys == pphys) nlog = plog + 1;
	else if (nphys == mphys) nlog = mlog + 1;
	else nlog = 0;
	if (nlog > 0xFFFF) { nphys++; nlog = 0; }
	*clk = (nphys << 16) | (nlog & 0xFFFF);
}

/* ---- shard: versioned key/value store ---- */
#define MV_CAP     512        /* key slots per shard */
#define MV_MAXVER  32         /* versions kept per key (newest first) */

struct mv_ver {
	uint64_t commit_ts;
	uint32_t value;
	uint64_t txn_id;
	int      committed;       /* 0 == staged by an in-flight txn */
};
struct mv_slot {
	uint32_t      key;
	int           used;
	int           nver;
	struct mv_ver ver[MV_MAXVER];   /* ver[0] newest */
};
struct mv_shard {
	struct mv_slot slot[MV_CAP];
	uint64_t       hlc;
};

/* ---- wire messages (memcpy into aligned locals on receipt) ---- */
enum { MV_READ = 'R', MV_STAT = 'Z' };         /* shard call */
struct shreq { uint8_t op; uint32_t key; uint64_t snap; };
struct shrep { int ok; uint32_t value; };

enum { MV_PREPARE = 'P', MV_COMMIT = 'C', MV_ABORT = 'A' };  /* shard cast */
struct shcast {
	uint8_t   op;
	uint64_t  txn_id;
	uint32_t  key;
	uint32_t  value;
	uint64_t  snap;
	uint64_t  commit_ts;
	uint64_t  low_water;      /* GC horizon: oldest live snapshot */
	xtc_pid_t coord;
};

enum { MV_BEGIN = 'B', MV_TXN = 'T', MV_RELEASE = 'L' };  /* coordinator call/cast */
struct coreq {
	uint8_t      op;
	uint64_t     snap;
	int          n;
	mvcc_write_t w[MVCC_MAX_WRITES];
};
struct corep { uint64_t snap; int committed; uint64_t commit_ts; };

enum { MV_VOTE = 'V' };                        /* coordinator cast */
struct covote { uint8_t op; uint64_t txn_id; int yes; xtc_pid_t shard; };
struct corel  { uint8_t op; uint64_t ts; };    /* snapshot release (cast) */

/* ---- module-global handles ---- */
static xtc_svr_t *g_shard_svr[MVCC_MAX_SHARDS];
static xtc_pid_t  g_shard_pid[MVCC_MAX_SHARDS];
static void      *g_shard_state[MVCC_MAX_SHARDS];
static int        g_n_shards;
static xtc_svr_t *g_coord_svr;
static xtc_pid_t  g_coord_pid;
static void      *g_coord_state;

int
mvcc_shard_of(uint32_t key)
{
	uint32_t h = key * 2654435761u;
	return (int)((h >> 16) % (uint32_t)(g_n_shards > 0 ? g_n_shards : 1));
}

/* ---- shard server ---- */
static struct mv_slot *
shard_find(struct mv_shard *s, uint32_t key, int create)
{
	uint32_t i = key % MV_CAP, n = 0;
	while (s->slot[i].used && s->slot[i].key != key) {
		i = (i + 1) % MV_CAP;
		if (++n >= MV_CAP) return NULL;
	}
	if (!s->slot[i].used) {
		if (!create) return NULL;
		s->slot[i].used = 1;
		s->slot[i].key = key;
		s->slot[i].nver = 0;
	}
	return &s->slot[i];
}

static void
shard_push_version(struct mv_slot *sl, uint64_t commit_ts, uint32_t value,
    uint64_t txn_id, int committed)
{
	int k;
	if (sl->nver < MV_MAXVER)
		sl->nver++;
	for (k = sl->nver - 1; k > 0; k--)
		sl->ver[k] = sl->ver[k - 1];
	sl->ver[0].commit_ts = commit_ts;
	sl->ver[0].value = value;
	sl->ver[0].txn_id = txn_id;
	sl->ver[0].committed = committed;
}

/*
 * Garbage-collect a key's version chain against `low_water` (the oldest
 * live snapshot).  Versions are newest-first, so the first committed
 * version with commit_ts <= low_water is the newest one any live
 * reader could see; every committed version older than it is invisible
 * to all live snapshots and is dropped.  Staged (uncommitted) versions
 * are newest and never pruned.  No epoch/RCU is needed: the shard proc
 * is the sole accessor of its chains (share-nothing).
 */
static void
shard_prune(struct mv_slot *sl, uint64_t low_water)
{
	int k;
	for (k = 0; k < sl->nver; k++)
		if (sl->ver[k].committed && sl->ver[k].commit_ts <= low_water) {
			if (k + 1 < sl->nver)
				sl->nver = k + 1;   /* drop older committed versions */
			return;
		}
}

static int
shard_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	struct mv_shard *s = st;
	struct shreq r;
	struct shrep rep = { 0, 0 };

	if (sz >= sizeof r) {
		memcpy(&r, req, sizeof r);
		if (r.op == MV_READ) {
			struct mv_slot *sl = shard_find(s, r.key, 0);
			if (sl != NULL) {
				int k;
				for (k = 0; k < sl->nver; k++)
					if (sl->ver[k].committed &&
					    sl->ver[k].commit_ts <= r.snap) {
						rep.ok = 1;
						rep.value = sl->ver[k].value;
						break;
					}
			}
		} else if (r.op == MV_STAT) {
			/* Total live versions in this shard (GC observability). */
			int i; uint32_t total = 0;
			for (i = 0; i < MV_CAP; i++)
				if (s->slot[i].used)
					total += (uint32_t)s->slot[i].nver;
			rep.ok = 1;
			rep.value = total;
		}
	}
	(void)xtc_svr_reply(call, &rep, sizeof rep);
	return XTC_SVR_CONTINUE;
}

static int
shard_handle_cast(void *st, const void *msg, size_t sz)
{
	struct mv_shard *s = st;
	struct shcast c;
	int i;

	if (sz < sizeof c)
		return XTC_SVR_CONTINUE;
	memcpy(&c, msg, sizeof c);

	if (c.op == MV_PREPARE) {
		struct mv_slot *sl = shard_find(s, c.key, 1);
		struct covote v;
		int yes = 1, k;

		hlc_observe(&s->hlc, c.commit_ts);
		if (sl == NULL) {
			yes = 0;
		} else {
			for (k = 0; k < sl->nver; k++) {
				/* Committed write newer than our snapshot, or
				 * another txn's staged write: conflict. */
				if (sl->ver[k].committed &&
				    sl->ver[k].commit_ts > c.snap)
					yes = 0;
				if (!sl->ver[k].committed &&
				    sl->ver[k].txn_id != c.txn_id)
					yes = 0;
			}
			if (yes)
				shard_push_version(sl, c.commit_ts, c.value,
				    c.txn_id, 0);
			/* Reclaim versions no live snapshot can see. */
			if (sl != NULL)
				shard_prune(sl, c.low_water);
		}
		v.op = MV_VOTE; v.txn_id = c.txn_id; v.yes = yes;
		v.shard = xtc_self();
		(void)xtc_svr_cast(c.coord, &v, sizeof v);
	} else if (c.op == MV_COMMIT || c.op == MV_ABORT) {
		/* Resolve every version this txn staged on this shard. */
		for (i = 0; i < MV_CAP; i++) {
			struct mv_slot *sl = &s->slot[i];
			int k;
			if (!sl->used)
				continue;
			for (k = 0; k < sl->nver; k++) {
				if (sl->ver[k].committed ||
				    sl->ver[k].txn_id != c.txn_id)
					continue;
				if (c.op == MV_COMMIT) {
					sl->ver[k].committed = 1;
				} else {
					int j;
					for (j = k; j < sl->nver - 1; j++)
						sl->ver[j] = sl->ver[j + 1];
					sl->nver--;
					k--;
				}
			}
		}
	}
	return XTC_SVR_CONTINUE;
}

/* ---- coordinator server ---- */
#define MV_MAX_TXN 64
struct mv_txn {
	int             active;
	uint64_t        txn_id;
	int             n_expected;   /* votes awaited == writes */
	int             n_votes;
	int             n_yes;
	uint64_t        commit_ts;
	xtc_svr_call_t *call;         /* deferred client commit call */
	int             n_shards;
	xtc_pid_t       shards[MVCC_MAX_SHARDS];
};
#define MV_MAX_LIVE 256
struct mv_coord {
	uint64_t      hlc;
	uint64_t      next_txn;
	struct mv_txn txn[MV_MAX_TXN];
	uint64_t      live[MV_MAX_LIVE];   /* outstanding read snapshots */
	int           n_live;
};

/* The GC horizon: the oldest live snapshot, or the current clock if no
 * snapshot is outstanding (then every committed version but the newest
 * per key is reclaimable). */
static uint64_t
coord_low_water(struct mv_coord *c)
{
	uint64_t lw;
	int i;
	if (c->n_live == 0)
		return c->hlc;
	lw = c->live[0];
	for (i = 1; i < c->n_live; i++)
		if (c->live[i] < lw)
			lw = c->live[i];
	return lw;
}

static void
coord_live_add(struct mv_coord *c, uint64_t ts)
{
	if (c->n_live < MV_MAX_LIVE)
		c->live[c->n_live++] = ts;
}
static void
coord_live_del(struct mv_coord *c, uint64_t ts)
{
	int i;
	for (i = 0; i < c->n_live; i++)
		if (c->live[i] == ts) {
			c->live[i] = c->live[--c->n_live];
			return;
		}
}

static struct mv_txn *
coord_txn_alloc(struct mv_coord *c)
{
	int i;
	for (i = 0; i < MV_MAX_TXN; i++)
		if (!c->txn[i].active) {
			memset(&c->txn[i], 0, sizeof c->txn[i]);
			c->txn[i].active = 1;
			return &c->txn[i];
		}
	return NULL;
}
static struct mv_txn *
coord_txn_find(struct mv_coord *c, uint64_t txn_id)
{
	int i;
	for (i = 0; i < MV_MAX_TXN; i++)
		if (c->txn[i].active && c->txn[i].txn_id == txn_id)
			return &c->txn[i];
	return NULL;
}

static void
coord_finish(struct mv_txn *t, int committed)
{
	struct corep rep;
	uint8_t fin[sizeof(struct shcast)];
	struct shcast f;
	int i;

	memset(&f, 0, sizeof f);
	f.op = committed ? MV_COMMIT : MV_ABORT;
	f.txn_id = t->txn_id;
	memcpy(fin, &f, sizeof f);
	for (i = 0; i < t->n_shards; i++)
		(void)xtc_svr_cast(t->shards[i], fin, sizeof f);

	rep.snap = 0;
	rep.committed = committed;
	rep.commit_ts = t->commit_ts;
	(void)xtc_svr_reply(t->call, &rep, sizeof rep);   /* frees saved call */
	t->active = 0;
}

static void
coord_add_shard(struct mv_txn *t, xtc_pid_t s)
{
	int i;
	for (i = 0; i < t->n_shards; i++)
		if (xtc_pid_eq(t->shards[i], s))
			return;
	if (t->n_shards < MVCC_MAX_SHARDS)
		t->shards[t->n_shards++] = s;
}

static int
coord_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	struct mv_coord *c = st;
	struct coreq r;
	struct corep rep = { 0, 0, 0 };
	struct mv_txn *t;
	int i;

	if (sz < sizeof(uint8_t)) {
		(void)xtc_svr_reply(call, &rep, sizeof rep);
		return XTC_SVR_CONTINUE;
	}
	memcpy(&r, req, sz < sizeof r ? sz : sizeof r);

	if (r.op == MV_BEGIN) {
		rep.snap = hlc_tick(&c->hlc);
		coord_live_add(c, rep.snap);     /* pin versions until released */
		(void)xtc_svr_reply(call, &rep, sizeof rep);
		return XTC_SVR_CONTINUE;
	}
	if (r.op != MV_TXN || r.n <= 0) {
		/* Empty transaction: trivially committed. */
		rep.committed = 1;
		rep.commit_ts = hlc_tick(&c->hlc);
		(void)xtc_svr_reply(call, &rep, sizeof rep);
		return XTC_SVR_CONTINUE;
	}
	if (r.n > MVCC_MAX_WRITES) {
		(void)xtc_svr_reply(call, &rep, sizeof rep);    /* abort */
		return XTC_SVR_CONTINUE;
	}
	t = coord_txn_alloc(c);
	if (t == NULL) {
		(void)xtc_svr_reply(call, &rep, sizeof rep);    /* table full */
		return XTC_SVR_CONTINUE;
	}
	t->txn_id = ++c->next_txn;
	t->commit_ts = hlc_tick(&c->hlc);     /* > any prior snapshot tick */
	t->n_expected = r.n;
	t->call = xtc_svr_call_save(call);    /* deferred reply */

	for (i = 0; i < r.n; i++)
		coord_add_shard(t, g_shard_pid[mvcc_shard_of(r.w[i].key)]);

	for (i = 0; i < r.n; i++) {
		struct shcast p;
		memset(&p, 0, sizeof p);
		p.op = MV_PREPARE; p.txn_id = t->txn_id;
		p.key = r.w[i].key; p.value = r.w[i].value;
		p.snap = r.snap; p.commit_ts = t->commit_ts;
		p.low_water = coord_low_water(c);
		p.coord = g_coord_pid;
		(void)xtc_svr_cast(g_shard_pid[mvcc_shard_of(r.w[i].key)],
		    &p, sizeof p);
	}
	return XTC_SVR_NOREPLY;                /* answer the client on the votes */
}

static int
coord_handle_cast(void *st, const void *msg, size_t sz)
{
	struct mv_coord *c = st;
	struct covote v;
	struct mv_txn *t;

	if (sz < 1)
		return XTC_SVR_CONTINUE;
	if (((const uint8_t *)msg)[0] == MV_RELEASE) {
		struct corel rel;
		if (sz >= sizeof rel) {
			memcpy(&rel, msg, sizeof rel);
			coord_live_del(c, rel.ts);
		}
		return XTC_SVR_CONTINUE;
	}
	if (sz < sizeof v)
		return XTC_SVR_CONTINUE;
	memcpy(&v, msg, sizeof v);
	if (v.op != MV_VOTE)
		return XTC_SVR_CONTINUE;
	t = coord_txn_find(c, v.txn_id);
	if (t == NULL)
		return XTC_SVR_CONTINUE;
	t->n_votes++;
	if (v.yes)
		t->n_yes++;
	if (t->n_votes >= t->n_expected)
		coord_finish(t, t->n_yes >= t->n_expected);
	return XTC_SVR_CONTINUE;
}

/* ---- bring-up / teardown ---- */
int
mvcc_start(xtc_loop_t **shard_loops, int n_shards, xtc_loop_t *coord_loop)
{
	xtc_svr_callbacks_t sh_cb = { NULL, shard_handle_call,
	    shard_handle_cast, NULL, NULL };
	xtc_svr_callbacks_t co_cb = { NULL, coord_handle_call,
	    coord_handle_cast, NULL, NULL };
	xtc_svr_opts_t opts = { 0 };
	struct mv_coord *cs;
	int i;

	if (n_shards <= 0 || n_shards > MVCC_MAX_SHARDS)
		return XTC_E_INVAL;
	g_n_shards = n_shards;

	for (i = 0; i < n_shards; i++) {
		struct mv_shard *ss = calloc(1, sizeof *ss);
		if (ss == NULL)
			return XTC_E_NOMEM;
		g_shard_state[i] = ss;
		opts.name = "mv.shard";
		if (xtc_svr_start(shard_loops[i], &sh_cb, ss, &opts,
		    &g_shard_svr[i]) != XTC_OK)
			return XTC_E_INTERNAL;
		g_shard_pid[i] = xtc_svr_pid(g_shard_svr[i]);
	}
	cs = calloc(1, sizeof *cs);
	if (cs == NULL)
		return XTC_E_NOMEM;
	g_coord_state = cs;
	opts.name = "mv.coord";
	if (xtc_svr_start(coord_loop, &co_cb, cs, &opts, &g_coord_svr) != XTC_OK)
		return XTC_E_INTERNAL;
	g_coord_pid = xtc_svr_pid(g_coord_svr);
	return XTC_OK;
}

void
mvcc_stop(void)
{
	int i;
	if (g_coord_svr != NULL)
		(void)xtc_svr_stop(g_coord_svr);
	for (i = 0; i < g_n_shards; i++)
		if (g_shard_svr[i] != NULL)
			(void)xtc_svr_stop(g_shard_svr[i]);
}

void
mvcc_fini(void)
{
	int i;
	/* Join each (already-stopped) server, freeing the server struct,
	 * then free the user state we allocated for it. */
	if (g_coord_svr != NULL) {
		(void)xtc_svr_join(g_coord_svr, 1LL * 1000 * 1000 * 1000);
		g_coord_svr = NULL;
	}
	free(g_coord_state);
	g_coord_state = NULL;
	for (i = 0; i < g_n_shards; i++) {
		if (g_shard_svr[i] != NULL) {
			(void)xtc_svr_join(g_shard_svr[i], 1LL * 1000 * 1000 * 1000);
			g_shard_svr[i] = NULL;
		}
		free(g_shard_state[i]);
		g_shard_state[i] = NULL;
	}
	g_n_shards = 0;
}

/* ---- client operations ---- */
static int
co_call(const void *req, size_t rsz, struct corep *out)
{
	void *r = NULL;
	size_t n = 0;
	int rc = xtc_svr_call(g_coord_pid, req, rsz, &r, &n,
	    5LL * 1000 * 1000 * 1000);
	if (rc == XTC_OK && r != NULL && n >= sizeof *out)
		memcpy(out, r, sizeof *out);
	else
		rc = (rc == XTC_OK) ? XTC_E_INTERNAL : rc;
	free(r);
	return rc;
}

uint64_t
mvcc_begin(void)
{
	struct coreq rq;
	struct corep rp;
	memset(&rq, 0, sizeof rq);
	rq.op = MV_BEGIN;
	if (co_call(&rq, sizeof rq.op + sizeof rq.snap + sizeof rq.n, &rp) != XTC_OK)
		return 0;
	return rp.snap;
}

int
mvcc_read(uint32_t key, uint64_t snap_ts, uint32_t *out)
{
	struct shreq rq = { MV_READ, key, snap_ts };
	struct shrep rp = { 0, 0 };
	void *r = NULL;
	size_t n = 0;
	int rc = xtc_svr_call(g_shard_pid[mvcc_shard_of(key)], &rq, sizeof rq,
	    &r, &n, 5LL * 1000 * 1000 * 1000);
	if (rc == XTC_OK && r != NULL && n >= sizeof rp)
		memcpy(&rp, r, sizeof rp);
	free(r);
	if (rc != XTC_OK)
		return rc;
	if (!rp.ok)
		return XTC_E_NOTFOUND;
	if (out != NULL)
		*out = rp.value;
	return XTC_OK;
}

void
mvcc_snapshot_release(uint64_t snap_ts)
{
	struct corel rel = { MV_RELEASE, snap_ts };
	(void)xtc_svr_cast(g_coord_pid, &rel, sizeof rel);
}

int
mvcc_total_versions(void)
{
	int i, total = 0;
	for (i = 0; i < g_n_shards; i++) {
		struct shreq rq = { MV_STAT, 0, 0 };
		struct shrep rp = { 0, 0 };
		void *r = NULL;
		size_t n = 0;
		if (xtc_svr_call(g_shard_pid[i], &rq, sizeof rq, &r, &n,
		    5LL * 1000 * 1000 * 1000) == XTC_OK && r != NULL &&
		    n >= sizeof rp) {
			memcpy(&rp, r, sizeof rp);
			total += (int)rp.value;
		}
		free(r);
	}
	return total;
}

int
mvcc_commit(uint64_t snap_ts, const mvcc_write_t *writes, int n,
    uint64_t *commit_ts)
{
	struct coreq rq;
	struct corep rp;
	int i, rc;

	if (n < 0 || n > MVCC_MAX_WRITES)
		return XTC_E_INVAL;
	memset(&rq, 0, sizeof rq);
	rq.op = MV_TXN;
	rq.snap = snap_ts;
	rq.n = n;
	for (i = 0; i < n; i++)
		rq.w[i] = writes[i];
	rc = co_call(&rq, sizeof rq, &rp);
	if (rc != XTC_OK)
		return rc;
	if (!rp.committed)
		return XTC_E_AGAIN;            /* conflict; retryable */
	if (commit_ts != NULL)
		*commit_ts = rp.commit_ts;
	return XTC_OK;
}
