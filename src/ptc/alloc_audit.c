/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/alloc_audit.c
 *	Debug allocation auditor.  See xtc_alloc_audit.h.
 *
 *	When enabled, the auditor installs an allocator hook that calls
 *	the previous (downstream) hook and records each live allocation
 *	-- pointer, size, and owning process (xtc_self at alloc time) --
 *	in a chained hash table.  free/realloc remove or re-key the
 *	record.  The table's own nodes are allocated through the
 *	downstream hook directly, so the auditor never audits itself.
 */

#include "xtc_int.h"
#include "xtc_alloc_audit.h"
#include "os_alloc.h"
#include "xtc_proc.h"

#include <pthread.h>
#include <string.h>

struct rec {
	const void  *ptr;
	size_t       size;
	xtc_pid_t    owner;
	struct rec  *next;
};

#define AUDIT_BUCKETS 16384u            /* power of two */

static pthread_mutex_t   g_mu = PTHREAD_MUTEX_INITIALIZER;
static int               g_on;
static struct __os_alloc_hook g_down;   /* downstream allocator */
static struct rec      **g_buckets;     /* AUDIT_BUCKETS, via g_down.malloc */
static size_t            g_live;
static size_t            g_live_bytes;

static size_t
__bucket(const void *p)
{
	uintptr_t v = (uintptr_t)p;
	v *= 0x9E3779B97F4A7C15ull;
	return (size_t)(v >> 32) & (AUDIT_BUCKETS - 1);
}

/* Insert a record (downstream alloc for the node, so it is not
 * itself audited).  Called with g_mu held. */
static void
__rec_insert(const void *p, size_t sz)
{
	struct rec *r;
	size_t b;
	if (p == NULL) return;
	r = g_down.malloc(sizeof *r);
	if (r == NULL) return;              /* audit best-effort under OOM */
	r->ptr = p;
	r->size = sz;
	r->owner = xtc_self();
	b = __bucket(p);
	r->next = g_buckets[b];
	g_buckets[b] = r;
	g_live++;
	g_live_bytes += sz;
}

/* Remove and free the record for p, if tracked.  Called with g_mu
 * held.  Returns the removed size (0 if untracked). */
static size_t
__rec_remove(const void *p)
{
	struct rec **pp, *r;
	size_t b;
	if (p == NULL) return 0;
	b = __bucket(p);
	for (pp = &g_buckets[b]; (r = *pp) != NULL; pp = &r->next) {
		if (r->ptr == p) {
			size_t sz = r->size;
			*pp = r->next;
			g_live--;
			g_live_bytes -= sz;
			g_down.free(r);
			return sz;
		}
	}
	return 0;
}

/* ---- wrapping hook ---- */
static void *
__a_malloc(size_t sz)
{
	void *p;
	(void)pthread_mutex_lock(&g_mu);
	p = g_down.malloc(sz);
	if (p != NULL) __rec_insert(p, sz);
	(void)pthread_mutex_unlock(&g_mu);
	return p;
}
static void *
__a_calloc(size_t n, size_t sz)
{
	void *p;
	(void)pthread_mutex_lock(&g_mu);
	p = g_down.calloc(n, sz);
	if (p != NULL) __rec_insert(p, n * sz);
	(void)pthread_mutex_unlock(&g_mu);
	return p;
}
static void *
__a_realloc(void *p, size_t sz)
{
	void *np;
	(void)pthread_mutex_lock(&g_mu);
	if (p != NULL) (void)__rec_remove(p);
	np = g_down.realloc(p, sz);
	if (np != NULL) __rec_insert(np, sz);
	(void)pthread_mutex_unlock(&g_mu);
	return np;
}
static void
__a_free(void *p)
{
	(void)pthread_mutex_lock(&g_mu);
	(void)__rec_remove(p);
	g_down.free(p);
	(void)pthread_mutex_unlock(&g_mu);
}
static void *
__a_aligned(size_t align, size_t sz)
{
	void *p;
	(void)pthread_mutex_lock(&g_mu);
	p = g_down.aligned(align, sz);
	if (p != NULL) __rec_insert(p, sz);
	(void)pthread_mutex_unlock(&g_mu);
	return p;
}
static void
__a_aligned_free(void *p)
{
	(void)pthread_mutex_lock(&g_mu);
	(void)__rec_remove(p);
	g_down.aligned_free(p);
	(void)pthread_mutex_unlock(&g_mu);
}

static const struct __os_alloc_hook g_audit_hook = {
	__a_malloc, __a_calloc, __a_realloc, __a_free,
	__a_aligned, __a_aligned_free
};

/* PUBLIC: int xtc_alloc_audit_enable __P((int)); */
int
xtc_alloc_audit_enable(int on)
{
	int rc = XTC_OK;
	(void)pthread_mutex_lock(&g_mu);
	if (on && !g_on) {
		(void)__os_alloc_get_hook(&g_down);   /* downstream */
		g_buckets = g_down.calloc(AUDIT_BUCKETS, sizeof *g_buckets);
		if (g_buckets == NULL) { rc = XTC_E_NOMEM; goto out; }
		g_live = 0;
		g_live_bytes = 0;
		(void)__os_alloc_set_hook(&g_audit_hook);
		g_on = 1;
	} else if (!on && g_on) {
		size_t b;
		(void)__os_alloc_set_hook(&g_down);   /* restore first */
		for (b = 0; b < AUDIT_BUCKETS; b++) {
			struct rec *r = g_buckets[b], *n;
			while (r != NULL) { n = r->next; g_down.free(r); r = n; }
		}
		g_down.free(g_buckets);
		g_buckets = NULL;
		g_on = 0;
	}
out:
	(void)pthread_mutex_unlock(&g_mu);
	return rc;
}

/* PUBLIC: void xtc_alloc_audit_stats __P((size_t *, size_t *)); */
void
xtc_alloc_audit_stats(size_t *out_count, size_t *out_bytes)
{
	(void)pthread_mutex_lock(&g_mu);
	if (out_count != NULL) *out_count = g_live;
	if (out_bytes != NULL) *out_bytes = g_live_bytes;
	(void)pthread_mutex_unlock(&g_mu);
}

/* PUBLIC: void xtc_alloc_audit_proc_leaks __P((xtc_pid_t, size_t *, size_t *)); */
void
xtc_alloc_audit_proc_leaks(xtc_pid_t pid, size_t *out_count, size_t *out_bytes)
{
	size_t b, cnt = 0, bytes = 0;
	(void)pthread_mutex_lock(&g_mu);
	if (g_on) {
		for (b = 0; b < AUDIT_BUCKETS; b++) {
			struct rec *r;
			for (r = g_buckets[b]; r != NULL; r = r->next) {
				if (xtc_pid_eq(r->owner, pid)) {
					cnt++;
					bytes += r->size;
				}
			}
		}
	}
	(void)pthread_mutex_unlock(&g_mu);
	if (out_count != NULL) *out_count = cnt;
	if (out_bytes != NULL) *out_bytes = bytes;
}
