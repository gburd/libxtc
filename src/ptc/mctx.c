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
};

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
 * Cleanups run before chunks so they can still touch them. */
static void
__free_chunks_and_cleanups(xtc_mctx_t *m)
{
	struct cleanup_entry *ce;
	struct mctx_chunk *c, *next;

	while ((ce = m->cleanups) != NULL) {
		m->cleanups = ce->next;
		ce->fn(ce->user);
		__os_free(ce);
	}
	for (c = m->first_chunk; c != NULL; c = next) {
		next = c->next;
		__os_free(c);
	}
	m->first_chunk = NULL;
	m->n_chunks = 0;
	m->n_bytes  = 0;
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
	xtc_mctx_t *child;
	if (m == NULL) return;
	/* Reset cascades: child contexts are reset before the parent's
	 * own chunks are freed.  This matches PG MemoryContextReset. */
	__lock(m);
	for (child = m->first_child; child != NULL; child = child->next_sibling)
		xtc_mctx_reset(child);
	__free_chunks_and_cleanups(m);
	__unlock(m);
}

/* ----- alloc / free ---------------------------------------------- */

void *
xtc_mctx_alloc(xtc_mctx_t *m, size_t size)
{
	struct mctx_chunk *c;
	if (m == NULL) return NULL;
	/* Overflow guard: CHUNK_HDR_SIZE + size must not wrap. */
	if (size > SIZE_MAX - CHUNK_HDR_SIZE) return NULL;
	if (__os_malloc(CHUNK_HDR_SIZE + size, (void **)&c) != XTC_OK ||
	    c == NULL) return NULL;
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
	__os_free(c);
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

/* A member pid is "gone" when xtc_proc_info reports NOTFOUND or !alive. */
static int
__grp_member_gone(xtc_pid_t pid)
{
	xtc_proc_info_t info;
	int rc = xtc_proc_info(pid, &info);
	if (rc != XTC_OK) return 1;      /* NOTFOUND -> reaped */
	return !info.alive;
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

	/* Snapshot the member list under the lock, then work outside it:
	 * the kill/wait can yield, and a member exiting on its own must be
	 * free to touch nothing of ours. */
	(void)__xtc_mtx_lock(&g->lock);
	n = g->n;
	if (n > 0) {
		if (__os_calloc((size_t)n, sizeof *snap, (void **)&snap) != XTC_OK) {
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
			__os_free(snap);
			return XTC_E_INVAL;
		}
	}

	/* Phase 1: kill every member with a deadline and confirm it is gone.
	 * A member wedged inside xtc_uncancelable (kill DEFERRED) may still
	 * be alive at its deadline -- record that, and do NOT discard. */
	for (i = 0; i < n; i++) {
		int status = XTC_KILL_TIMEOUT;
		if (__grp_member_gone(snap[i]))
			continue;
		(void)xtc_exit_pid_deadline(snap[i], reason, timeout_ns, &status);
		if (!__grp_member_gone(snap[i]))
			all_gone = 0;   /* still alive: a deferred/wedged kill */
	}

	/* Phase 2: ONLY if every member is confirmed gone, reset the arena.
	 * Discarding state a live fiber may still touch is exactly the
	 * corruption this primitive exists to avoid. */
	if (all_gone) {
		xtc_mctx_reset(g->arena);
		/* The members are dead; forget them so the group can be reused
		 * (a fresh cohort re-adds itself). */
		(void)__xtc_mtx_lock(&g->lock);
		g->n = 0;
		(void)__xtc_mtx_unlock(&g->lock);
	}

	__os_free(snap);
	if (out_all_gone != NULL) *out_all_gone = all_gone;
	return rc;
}
