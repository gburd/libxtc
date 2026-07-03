/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/reg.c
 *	Process registry implementation.  M10.5: a chained FNV-1a hash
 *	table (name -> xtc_pid_t) under a single mutex.  Lookup,
 *	register, and unregister are O(1) average.  This is the same
 *	fixed-size, separately-chained, single-mutex model used by
 *	src/ptc/lock_mgr.c and src/ptc/alloc_audit.c.  A future M11+
 *	step may add RCU-protected lockless reads (see Option B in
 *	.agent/M_REGISTRY_HASH.md); the node/bucket layout here is a
 *	strict subset of that, so no rework is required.
 */

#include "xtc_int.h"
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock: preemption-safe locks */
#include "xtc_reg.h"
#include "xtc_sim.h"       /* XTC_SIM_BUGGIFY / xtc_sim_fault (DST) */

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/*
 * Fixed power-of-two bucket count, no resize -- matches lock_mgr.c's
 * locker_table[256] and the named-service workload the registry
 * serves (tens to hundreds of long-lived names).
 *
 * ponytail: fixed 256 buckets, no resize.  If a deployment ever
 * registers tens of thousands of names, bump REG_NBUCKETS or add a
 * one-shot grow-on-first-overflow -- but only when a real workload
 * shows chains long enough to matter.
 */
#define REG_NBUCKETS 256u               /* power of two */

struct reg_node {
	struct reg_node *next;          /* collision chain link */
	uint32_t         hash;          /* cached FNV-1a, fast chain reject */
	xtc_pid_t        pid;
	char            *name;          /* __os_strdup'd, owned by node */
};

struct xtc_reg {
	pthread_mutex_t   lock;
	struct reg_node **buckets;      /* REG_NBUCKETS heads, zeroed */
	int               n;            /* live entry count */
};

/* FNV-1a over a NUL-terminated name; same constants as lock_mgr.c.
 * 0 is a fine result -- the bucket index masks it. */
static uint32_t
__reg_hash(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s != '\0'; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

int
xtc_reg_create(xtc_reg_t **out)
{
	xtc_reg_t *r;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK) return rc;
	rc = __os_calloc(REG_NBUCKETS, sizeof *r->buckets, (void **)&r->buckets);
	if (rc != XTC_OK) {
		__os_free(r);
		return rc;
	}
	(void)pthread_mutex_init(&r->lock, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_reg_destroy(xtc_reg_t *r)
{
	uint32_t b;
	if (r == NULL) return;
	for (b = 0; b < REG_NBUCKETS; b++) {
		struct reg_node *node = r->buckets[b], *next;
		while (node != NULL) {
			next = node->next;
			__os_free(node->name);
			__os_free(node);
			node = next;
		}
	}
	__os_free(r->buckets);
	(void)pthread_mutex_destroy(&r->lock);
	__os_free(r);
}

/* Find a node by name in its bucket.  Called with r->lock held. */
static struct reg_node *
__reg_find_locked(struct xtc_reg *r, const char *name, uint32_t h)
{
	struct reg_node *node;
	for (node = r->buckets[h & (REG_NBUCKETS - 1)]; node != NULL;
	    node = node->next)
		if (node->hash == h && strcmp(node->name, name) == 0)
			return node;
	return NULL;
}

int
xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	struct reg_node *node;
	uint32_t h, b;
	int rc = XTC_OK;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	h = __reg_hash(name);
	b = h & (REG_NBUCKETS - 1);
	(void)__xtc_mtx_lock(&r->lock);
	if (__reg_find_locked(r, name, h) != NULL) { rc = XTC_E_INVAL; goto out; }
	if ((rc = __os_calloc(1, sizeof *node, (void **)&node)) != XTC_OK)
		goto out;
	if ((rc = __os_strdup(name, &node->name)) != XTC_OK) {
		__os_free(node);
		goto out;
	}
	node->hash = h;
	node->pid = pid;
	node->next = r->buckets[b];      /* head-insert */
	r->buckets[b] = node;
	r->n++;
out:
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_reg_unregister(xtc_reg_t *r, const char *name)
{
	struct reg_node **link, *node;
	uint32_t h;
	int rc = XTC_E_INVAL;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	h = __reg_hash(name);
	(void)__xtc_mtx_lock(&r->lock);
	for (link = &r->buckets[h & (REG_NBUCKETS - 1)]; (node = *link) != NULL;
	    link = &node->next) {
		if (node->hash == h && strcmp(node->name, name) == 0) {
			*link = node->next;      /* unlink by link pointer */
			__os_free(node->name);
			__os_free(node);
			r->n--;
			rc = XTC_OK;
			break;
		}
	}
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid)
{
	struct reg_node *node;
	int rc = XTC_E_INVAL;
	if (r == NULL || name == NULL || out_pid == NULL) return XTC_E_INVAL;
	/*
	 * Buggify: under DST, occasionally report a registered name as
	 * transiently NOT FOUND (a fresh per-call fault draw so a retrying
	 * caller eventually resolves it; the site coin gates liveness).  A
	 * lookup is a hint that may race a concurrent unregister, so a
	 * transient miss is a legal outcome callers already retry -- this
	 * stresses that retry path deterministically.  Zero cost when sim
	 * is inactive.
	 */
	if (XTC_SIM_BUGGIFY("reg.whereis.transient_miss") && xtc_sim_fault(250))
		return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	node = __reg_find_locked(r, name, __reg_hash(name));
	if (node != NULL) {
		*out_pid = node->pid;
		rc = XTC_OK;
	}
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_reg_count(const xtc_reg_t *r)
{
	int n;
	if (r == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&r->lock);
	n = r->n;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&r->lock);
	return n;
}
