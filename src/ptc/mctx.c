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
#include "xtc_mctx.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct chunk {
	struct chunk *prev;
	struct chunk *next;
	xtc_mctx_t   *owner;
	size_t        size;
	/* user payload follows */
};

#define CHUNK_HDR_SIZE   (sizeof(struct chunk))
#define CHUNK_TO_PTR(c)  ((void *)((char *)(c) + CHUNK_HDR_SIZE))
#define PTR_TO_CHUNK(p)  ((struct chunk *)((char *)(p) - CHUNK_HDR_SIZE))

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

	struct chunk        *first_chunk;
	struct cleanup_entry *cleanups;

	size_t           n_chunks;
	size_t           n_bytes;
};

static void
__lock(xtc_mctx_t *m)
{
	if (m->has_lock) (void)pthread_mutex_lock(&m->lock);
}
static void
__unlock(xtc_mctx_t *m)
{
	if (m->has_lock) (void)pthread_mutex_unlock(&m->lock);
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
	struct chunk *c, *next;

	while ((ce = m->cleanups) != NULL) {
		m->cleanups = ce->next;
		ce->fn(ce->user);
		free(ce);
	}
	for (c = m->first_chunk; c != NULL; c = next) {
		next = c->next;
		free(c);
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
	struct chunk *c;
	if (m == NULL) return NULL;
	/* Overflow guard: CHUNK_HDR_SIZE + size must not wrap. */
	if (size > SIZE_MAX - CHUNK_HDR_SIZE) return NULL;
	c = malloc(CHUNK_HDR_SIZE + size);
	if (c == NULL) return NULL;
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
	struct chunk *c;
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
	free(c);
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
	if (m->has_lock) (void)pthread_mutex_lock((pthread_mutex_t *)&m->lock);
	v = m->n_bytes;
	if (m->has_lock) (void)pthread_mutex_unlock((pthread_mutex_t *)&m->lock);
	return v;
}

size_t
xtc_mctx_total_chunks(const xtc_mctx_t *m)
{
	size_t v;
	if (m == NULL) return 0;
	if (m->has_lock) (void)pthread_mutex_lock((pthread_mutex_t *)&m->lock);
	v = m->n_chunks;
	if (m->has_lock) (void)pthread_mutex_unlock((pthread_mutex_t *)&m->lock);
	return v;
}
