/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/mctx.c
 *	Memory contexts: simple-but-correct first cut.  Every alloc is
 *	a malloc with a 32-byte header that chains onto a doubly-linked
 *	free-list owned by the context.  Destroy/reset frees the chain
 *	in O(N).  M11.5 will replace this with slab caches for the
 *	common fixed-size paths.
 *
 *	Locking: optional pthread_mutex per context, controlled by
 *	XTC_MCTX_THREAD_SAFE at create time.  Locks are recursive-
 *	style only on the parent->child relationship walk (we never
 *	hold a child's lock while taking a parent's, and vice versa).
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_mctx.h"
#include "xtc_res.h"      /* optional MEM_BYTES metering: xtc_res_attach_mctx */
#include "xtc_proc.h"     /* arena groups: xtc_self / xtc_exit_pid_deadline */
#include "xtc_inspect.h" /* xtc_proc_info: is a member still alive? */
#include <stdio.h>       /* snprintf for the arena name */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct mctx_chunk {
	struct mctx_chunk *prev;
	struct mctx_chunk *next;
	xtc_mctx_t   *owner;
	size_t        size;
	/* user payload follows */
};

#define CHUNK_HDR_SIZE   (sizeof(struct mctx_chunk))
#define CHUNK_TO_PTR(c)  ((void *)((char *)(c) + CHUNK_HDR_SIZE))
#define PTR_TO_CHUNK(p)  ((struct mctx_chunk *)((char *)(p) - CHUNK_HDR_SIZE))

struct cleanup_entry {
	xtc_mctx_cleanup_fn fn;
	void               *user;
	struct cleanup_entry *next;
};

struct xtc_mctx {
	char            *name;
	unsigned         flags;
	pthread_mutex_t  lock;        /* only meaningful if THREAD_SAFE */
	int              has_lock;

	xtc_mctx_t      *parent;
	xtc_mctx_t      *first_child;
	xtc_mctx_t      *prev_sibling;
	xtc_mctx_t      *next_sibling;

	struct mctx_chunk        *first_chunk;
	struct cleanup_entry *cleanups;

	size_t           n_chunks;
	size_t           n_bytes;

	/* Optional MEM_BYTES accountant (xtc_res_attach_mctx; inherited
	 * by children created while set).  While non-NULL it has been
	 * charged exactly __mctx_footprint(m): every live chunk's header
	 * plus payload.  NULL -- the default -- meters nothing.  Attach
	 * is a setup-time call: it must not race an alloc/free on m. */
	xtc_res_t       *res;
};

/* Heap bytes this context's live chunks occupy (header + payload). */
static int64_t
__mctx_footprint(const xtc_mctx_t *m)
{
	return (int64_t)(m->n_bytes + m->n_chunks * CHUNK_HDR_SIZE);
}

static void
__lock(xtc_mctx_t *m)
{
	if (m->has_lock) (void)__xtc_mtx_lock(&m->lock);
}
static void
__unlock(xtc_mctx_t *m)
{
	if (m->has_lock) (void)__xtc_mtx_unlock(&m->lock);
}

/* ----- create / destroy ------------------------------------------ */

int
xtc_mctx_create(xtc_mctx_t *parent, const char *name,
                unsigned flags, xtc_mctx_t **out)
{
	xtc_mctx_t *m;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *m, (void **)&m)) != XTC_OK)
		return rc;
	if (name != NULL) {
		if ((rc = __os_strdup(name, &m->name)) != XTC_OK) {
			__os_free(m);
			return rc;
		}
	}
	m->flags = flags;
	if (flags & XTC_MCTX_THREAD_SAFE) {
		(void)pthread_mutex_init(&m->lock, NULL);
		m->has_lock = 1;
	}
	if (parent != NULL) {
		__lock(parent);
		m->parent = parent;
		m->next_sibling = parent->first_child;
		if (parent->first_child != NULL)
			parent->first_child->prev_sibling = m;
		parent->first_child = m;
		m->res = parent->res;
		__unlock(parent);
	}
	*out = m;
	return XTC_OK;
}

/* Detach `m` from its parent's child list.  Caller must hold no
 * locks; we acquire the parent's lock if needed. */
static void
__detach(xtc_mctx_t *m)
{
	xtc_mctx_t *p = m->parent;
	if (p == NULL) return;
	__lock(p);
	if (m->prev_sibling) m->prev_sibling->next_sibling = m->next_sibling;
	else                 p->first_child                = m->next_sibling;
	if (m->next_sibling) m->next_sibling->prev_sibling = m->prev_sibling;
	__unlock(p);
	m->parent = m->prev_sibling = m->next_sibling = NULL;
}

/* Free chunks + cleanups for a single context (no recursion).
 * Cleanups run before chunks so they can still touch them.
 *
 * NO LOCK IS HELD WHILE A CLEANUP CALLBACK RUNS.  Each list is
 * DETACHED under the context lock, then walked with the lock dropped,
 * because a callback may legitimately call back into its own context
 * (xtc_mctx_total_bytes/_total_chunks take the same mutex, which is not
 * recursive -- holding it across the callback was a self-deadlock).
 * Detaching also keeps the callback's view coherent: the cleanups are
 * unlinked, but every chunk is still attached and live, which is the
 * documented "cleanups run before the chunks are freed" ordering.
 * Callers must hold no context lock. */
static void
__free_chunks_and_cleanups(xtc_mctx_t *m)
{
	struct cleanup_entry *ce, *ce_next;
	struct mctx_chunk *c, *next;
	int64_t fp;

	__lock(m);
	ce = m->cleanups;
	m->cleanups = NULL;
	__unlock(m);

	for (; ce != NULL; ce = ce_next) {
		ce_next = ce->next;
		ce->fn(ce->user);
		__os_free(ce);
	}

	__lock(m);
	c = m->first_chunk;
	fp = __mctx_footprint(m);
	m->first_chunk = NULL;
	m->n_chunks = 0;
	m->n_bytes  = 0;
	__unlock(m);

	for (; c != NULL; c = next) {
		next = c->next;
		__os_free(c);
	}
	if (m->res != NULL)
		xtc_res_release(m->res, XTC_RES_MEM_BYTES, fp);
}

void
xtc_mctx_destroy(xtc_mctx_t *m)
{
	xtc_mctx_t *child, *next;
	if (m == NULL) return;

	/* Recursively destroy children first.  We grab the lock to
	 * snapshot the head pointer; child destroys re-detach
	 * themselves. */
	for (;;) {
		__lock(m);
		child = m->first_child;
		__unlock(m);
		if (child == NULL) break;
		xtc_mctx_destroy(child);
	}

	__detach(m);

	/* Free the children list pointers in case any exotic destroy
	 * path didn't go through detach. */
	for (child = m->first_child; child != NULL; child = next) {
		next = child->next_sibling;
		xtc_mctx_destroy(child);
	}

	__free_chunks_and_cleanups(m);

	if (m->has_lock) (void)pthread_mutex_destroy(&m->lock);
	if (m->name) __os_free(m->name);
	__os_free(m);
}

void
xtc_mctx_reset(xtc_mctx_t *m)
{
	xtc_mctx_t *child, *next;
	if (m == NULL) return;
	/* Reset cascades: child contexts are reset before the parent's own
	 * chunks are freed.  This matches PG MemoryContextReset.
	 *
	 * m's lock is taken only to read a sibling link and is DROPPED
	 * across the recursion and the cleanup callbacks: a cleanup may
	 * introspect its own context or an ancestor, and these mutexes are
	 * not recursive. */
	__lock(m);
	child = m->first_child;
	__unlock(m);
	for (; child != NULL; child = next) {
		xtc_mctx_reset(child);
		__lock(m);
		next = child->next_sibling;
		__unlock(m);
	}
	__free_chunks_and_cleanups(m);
}

/* ----- alloc / free ---------------------------------------------- */

void *
xtc_mctx_alloc(xtc_mctx_t *m, size_t size)
{
	struct mctx_chunk *c;
	int64_t charge;
	if (m == NULL) return NULL;
	/* Overflow guards: CHUNK_HDR_SIZE + size must not wrap, and the
	 * metered charge must fit an int64_t.  Then charge BEFORE
	 * allocating (as the slab does): a refused charge never touches
	 * the heap. */
	if (size > SIZE_MAX - CHUNK_HDR_SIZE) return NULL;
	if (size > (size_t)INT64_MAX - CHUNK_HDR_SIZE) return NULL;
	charge = (int64_t)(CHUNK_HDR_SIZE + size);
	if (m->res != NULL &&
	    xtc_res_acquire(m->res, XTC_RES_MEM_BYTES, charge) != XTC_OK)
		return NULL;
	if (__os_malloc(CHUNK_HDR_SIZE + size, (void **)&c) != XTC_OK) {
		if (m->res != NULL)
			xtc_res_release(m->res, XTC_RES_MEM_BYTES, charge);
		return NULL;
	}
	c->owner = m;
	c->size  = size;

	__lock(m);
	c->prev = NULL;
	c->next = m->first_chunk;
	if (m->first_chunk != NULL) m->first_chunk->prev = c;
	m->first_chunk = c;
	m->n_chunks++;
	m->n_bytes += size;
	__unlock(m);

	return CHUNK_TO_PTR(c);
}

void *
xtc_mctx_calloc(xtc_mctx_t *m, size_t n, size_t size)
{
	size_t total = n * size;
	void *p;
	if (n != 0 && total / n != size) return NULL;   /* overflow */
	p = xtc_mctx_alloc(m, total);
	if (p != NULL) memset(p, 0, total);
	return p;
}

void *
xtc_mctx_strdup(xtc_mctx_t *m, const char *s)
{
	size_t len;
	char *p;
	if (s == NULL) return NULL;
	len = strlen(s) + 1;
	p = xtc_mctx_alloc(m, len);
	if (p != NULL) memcpy(p, s, len);
	return p;
}

void
xtc_mctx_free(xtc_mctx_t *m, void *p)
{
	struct mctx_chunk *c;
	if (p == NULL) return;
	c = PTR_TO_CHUNK(p);
	if (m == NULL) m = c->owner;
	__lock(m);
	if (c->prev) c->prev->next = c->next;
	else         m->first_chunk = c->next;
	if (c->next) c->next->prev = c->prev;
	m->n_chunks--;
	m->n_bytes -= c->size;
	__unlock(m);
	if (m->res != NULL)
		xtc_res_release(m->res, XTC_RES_MEM_BYTES,
		    (int64_t)(CHUNK_HDR_SIZE + c->size));
	__os_free(c);
}

/* ----- MEM_BYTES metering (declared in xtc_res.h) ----------------- */

int
xtc_res_attach_mctx(xtc_res_t *r, xtc_mctx_t *m)
{
	int64_t fp;
	int rc;
	if (m == NULL) return XTC_E_INVAL;
	__lock(m);
	fp = __mctx_footprint(m);
	if (r != NULL && r != m->res &&
	    (rc = xtc_res_acquire(r, XTC_RES_MEM_BYTES, fp)) != XTC_OK) {
		__unlock(m);
		return rc;   /* already over the cap: nothing changes */
	}
	if (m->res != NULL && m->res != r)
		xtc_res_release(m->res, XTC_RES_MEM_BYTES, fp);
	m->res = r;
	__unlock(m);
	return XTC_OK;
}

/* ----- cleanup callbacks ----------------------------------------- */

int
xtc_mctx_register_cleanup(xtc_mctx_t *m, xtc_mctx_cleanup_fn fn, void *user)
{
	struct cleanup_entry *ce;
	int rc;
	if (m == NULL || fn == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *ce, (void **)&ce)) != XTC_OK) return rc;
	ce->fn = fn;
	ce->user = user;
	__lock(m);
	ce->next = m->cleanups;
	m->cleanups = ce;
	__unlock(m);
	return XTC_OK;
}

/* ----- introspection --------------------------------------------- */

const char *
xtc_mctx_name(const xtc_mctx_t *m)
{
	return (m && m->name) ? m->name : "(unnamed)";
}

size_t
xtc_mctx_total_bytes(const xtc_mctx_t *m)
{
	size_t v;
	if (m == NULL) return 0;
	if (m->has_lock) (void)__xtc_mtx_lock((pthread_mutex_t *)&m->lock);
	v = m->n_bytes;
	if (m->has_lock) (void)__xtc_mtx_unlock((pthread_mutex_t *)&m->lock);
	return v;
}

size_t
xtc_mctx_total_chunks(const xtc_mctx_t *m)
{
	size_t v;
	if (m == NULL) return 0;
	if (m->has_lock) (void)__xtc_mtx_lock((pthread_mutex_t *)&m->lock);
	v = m->n_chunks;
	if (m->has_lock) (void)__xtc_mtx_unlock((pthread_mutex_t *)&m->lock);
	return v;
}

/* ================= arena groups: shared-state discard on kill ==========
 *
 * A recovery boundary smaller than the whole process: a set of member
 * fibers plus one arena (mctx).  discard() kills every member, waits
 * until ALL are confirmed gone, then resets the arena -- so the shared
 * state a killed fiber was mid-mutating is thrown away with nothing
 * alive to still be touching it.  See xtc_mctx.h.
 */

struct xtc_arena_group {
	xtc_mctx_t     *arena;      /* the discardable arena */
	pthread_mutex_t lock;       /* guards the member list (multi-carrier) */
	xtc_pid_t      *members;    /* dynamic array */
	int             n;
	int             cap;
	int             sealed;     /* a discard is in flight; joins refused */
};

int
xtc_arena_group_create(const char *name, xtc_arena_group_t **out)
{
	xtc_arena_group_t *g;
	char nbuf[64];
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *g, (void **)&g)) != XTC_OK)
		return rc;
	/* THREAD_SAFE: members allocate into the arena from different
	 * carrier threads, so it must be internally locked. */
	snprintf(nbuf, sizeof nbuf, "%s", name != NULL ? name : "arena-group");
	if ((rc = xtc_mctx_create(NULL, nbuf, XTC_MCTX_THREAD_SAFE,
	    &g->arena)) != XTC_OK) {
		__os_free(g);
		return rc;
	}
	if (pthread_mutex_init(&g->lock, NULL) != 0) {
		xtc_mctx_destroy(g->arena);
		__os_free(g);
		return XTC_E_INTERNAL;
	}
	*out = g;
	return XTC_OK;
}

void
xtc_arena_group_destroy(xtc_arena_group_t *g)
{
	if (g == NULL) return;
	xtc_mctx_destroy(g->arena);
	(void)pthread_mutex_destroy(&g->lock);
	__os_free(g->members);
	__os_free(g);
}

xtc_mctx_t *
xtc_arena_group_mctx(xtc_arena_group_t *g)
{
	return g != NULL ? g->arena : NULL;
}

int
xtc_arena_group_add(xtc_arena_group_t *g)
{
	xtc_pid_t self = xtc_self();
	int i, rc = XTC_OK;
	if (g == NULL) return XTC_E_INVAL;
	if (xtc_pid_is_none(self)) return XTC_E_INVAL;   /* a fiber adds itself */
	(void)__xtc_mtx_lock(&g->lock);
	/* SEALED: a discard is in flight and is about to reset the arena.
	 * Admitting a member now is exactly the bug the seal exists to stop
	 * -- the joiner would allocate shared state into memory that the
	 * reset is about to throw away, and (having joined after the
	 * discard's snapshot) would not even be killed first.  Refuse with
	 * XTC_E_AGAIN: the group is unsealed once the discard reaches a
	 * verdict, so a FRESH cohort may then join, but THIS caller must not
	 * touch the arena -- it belongs to the cohort being discarded. */
	if (g->sealed) { rc = XTC_E_AGAIN; goto out; }
	for (i = 0; i < g->n; i++)                       /* idempotent per pid */
		if (xtc_pid_eq(g->members[i], self)) goto out;
	if (g->n == g->cap) {
		int ncap = g->cap == 0 ? 8 : g->cap * 2;
		void *np = NULL;
		if ((rc = __os_realloc(g->members, sizeof(xtc_pid_t) * (size_t)ncap,
		    &np)) != XTC_OK)
			goto out;
		g->members = np;
		g->cap = ncap;
	}
	g->members[g->n++] = self;
out:
	(void)__xtc_mtx_unlock(&g->lock);
	return rc;
}

int
xtc_arena_group_size(xtc_arena_group_t *g)
{
	int n;
	if (g == NULL) return 0;
	(void)__xtc_mtx_lock(&g->lock);
	n = g->n;
	(void)__xtc_mtx_unlock(&g->lock);
	return n;
}

/*
 * A member pid is "gone" only when xtc_proc_info reports NOTFOUND, i.e.
 * its proc-table slot has been released.
 *
 * !alive is deliberately NOT accepted as proof.  A proc clears `alive`
 * BEFORE running its at-exit callbacks, and such a callback may still
 * read or write the group's arena (releasing a lock, resetting a
 * context, flushing a buffer).  Treating !alive as gone let discard
 * reset the arena underneath a running exit hook.  The slot is released
 * only after every at-exit callback has returned, so NOTFOUND is the
 * first moment nothing of the member can touch arena memory.
 */
static int
__grp_member_gone(xtc_pid_t pid)
{
	xtc_proc_info_t info;
	return xtc_proc_info(pid, &info) != XTC_OK;
}

/*
 * Poll until `pid` is reaped or `timeout_ns` elapses; 1 if reaped.
 * Polling (not a condvar) for the same reason xtc_exit_pid_deadline
 * polls: the target acts on its own fiber and a wedged target signals
 * nothing.  Yields on a fiber, sleeps the OS thread off one.
 */
static int
__grp_wait_reaped(xtc_pid_t pid, int64_t timeout_ns)
{
	int64_t start = 0, now = 0, slice = 50000;   /* 50us, backing off */
	int on_fiber = !xtc_pid_is_none(xtc_self());

	(void)__os_clock_mono(&start);
	for (;;) {
		if (__grp_member_gone(pid))
			return 1;
		(void)__os_clock_mono(&now);
		if (timeout_ns <= 0 || now - start >= timeout_ns)
			return 0;
		if (on_fiber)
			(void)xtc_proc_sleep(slice);
		else
			(void)__os_sleep_ns(slice);
		if (slice < 1000000)
			slice *= 2;
	}
}

/* Reopen the group to joins.  Paired with the seal taken by discard. */
static void
__grp_unseal(xtc_arena_group_t *g)
{
	(void)__xtc_mtx_lock(&g->lock);
	g->sealed = 0;
	(void)__xtc_mtx_unlock(&g->lock);
}

int
xtc_arena_group_discard(xtc_arena_group_t *g, int reason,
                        int64_t timeout_ns, int *out_all_gone)
{
	xtc_pid_t *snap = NULL;
	xtc_pid_t self;
	int n, i, all_gone = 1, rc = XTC_OK;

	if (g == NULL) return XTC_E_INVAL;
	self = xtc_self();

	/* SEAL the group and snapshot the member list in the SAME lock hold,
	 * then work outside the lock (the kill/wait yields, and a member
	 * exiting on its own must be free to touch nothing of ours).
	 *
	 * Seal-with-snapshot is what makes the snapshot AUTHORITATIVE: every
	 * add that got in before this point is in `snap`, and every add after
	 * it fails with XTC_E_AGAIN.  Without the seal a fiber could join
	 * during the yielding waits below -- after the snapshot, so never
	 * killed -- and then hold pointers into an arena the reset had wiped,
	 * while also being silently dropped from the member count.  A mere
	 * re-check of the count before the reset would NOT fix that: a join
	 * can still land between the check and the reset. */
	(void)__xtc_mtx_lock(&g->lock);
	if (g->sealed) {          /* another discard is already in flight */
		(void)__xtc_mtx_unlock(&g->lock);
		return XTC_E_AGAIN;
	}
	g->sealed = 1;
	n = g->n;
	if (n > 0) {
		if (__os_calloc((size_t)n, sizeof *snap, (void **)&snap) != XTC_OK) {
			g->sealed = 0;
			(void)__xtc_mtx_unlock(&g->lock);
			return XTC_E_NOMEM;
		}
		memcpy(snap, g->members, sizeof(xtc_pid_t) * (size_t)n);
	}
	(void)__xtc_mtx_unlock(&g->lock);

	/* A member must not discard its own group: it would kill itself
	 * mid-wait and never reach the reset. */
	for (i = 0; i < n; i++) {
		if (!xtc_pid_is_none(self) && xtc_pid_eq(snap[i], self)) {
			__grp_unseal(g);
			__os_free(snap);
			return XTC_E_INVAL;
		}
	}

	/* Phase 1: kill every member with a deadline and confirm it is gone.
	 * A member wedged inside xtc_uncancelable (kill DEFERRED) may still
	 * be alive at its deadline -- record that, and do NOT discard. */
	for (i = 0; i < n; i++) {
		int status = XTC_KILL_TIMEOUT;
		int64_t t0 = 0, t1 = 0, left;
		if (__grp_member_gone(snap[i]))
			continue;
		(void)__os_clock_mono(&t0);
		(void)xtc_exit_pid_deadline(snap[i], reason, timeout_ns, &status);
		/* Wedged inside a mask with the kill latched: it cannot act on
		 * the kill while the mask is held, so waiting longer cannot
		 * change the answer. */
		if (status == XTC_KILL_DEFERRED) {
			all_gone = 0;
			continue;
		}
		/* exit_pid_deadline reports DELIVERED as soon as the target is
		 * !alive -- which is BEFORE its at-exit callbacks have run, and
		 * those may still touch the arena.  Spend what is left of this
		 * member's budget waiting for the proc-table slot to be
		 * released: that is the first instant the member is provably
		 * unable to reach arena memory. */
		(void)__os_clock_mono(&t1);
		left = timeout_ns - (t1 - t0);
		if (left <= 0)
			left = 1;   /* one non-blocking probe */
		if (!__grp_wait_reaped(snap[i], left))
			all_gone = 0;   /* still alive, or still unwinding */
	}

	/* Phase 2: ONLY if every member is confirmed gone, reset the arena.
	 * Discarding state a live fiber may still touch is exactly the
	 * corruption this primitive exists to avoid. */
	if (all_gone) {
		/* Still sealed here, so nothing can have joined since the
		 * snapshot and the reset cannot strand a live member. */
		xtc_mctx_reset(g->arena);
		/* The members are dead; forget them so the group can be reused
		 * (a fresh cohort re-adds itself). */
		(void)__xtc_mtx_lock(&g->lock);
		g->n = 0;
		(void)__xtc_mtx_unlock(&g->lock);
	}

	/* Verdict reached: reopen the group.  Unsealing after the reset (not
	 * before) is what keeps the reset atomic with respect to joins. */
	__grp_unseal(g);
	__os_free(snap);
	if (out_all_gone != NULL) *out_all_gone = all_gone;
	return rc;
}
