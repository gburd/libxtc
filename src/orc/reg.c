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
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_reg.h"
#include "xtc_proc.h"      /* reaper proc: monitor + recv + drop-on-DOWN */
#include "xtc_svr.h"       /* xtc_svr_call_name via-dispatch */
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
	int              dup;           /* 1 = duplicate-key (pub/sub) entry */
};

struct xtc_reg {
	pthread_mutex_t   lock;
	struct reg_node **buckets;      /* REG_NBUCKETS heads, zeroed */
	int               n;            /* live entry count */
	_Atomic int       has_reaper;   /* 1 once a reaper proc has registered */
	xtc_pid_t         reaper;       /* the reaper's pid (valid if has_reaper) */
};

/* Wire message a caller sends to the reaper proc: "monitor `pid`; when
 * it goes DOWN, drop it from every key."  Fixed layout, sent by value. */
struct reg_reaper_msg {
	uint32_t  tag;                  /* REG_REAPER_MONITOR */
	xtc_pid_t pid;
};
#define REG_REAPER_MONITOR 0x9E1u

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

/*
 * Duplicate-key (pub/sub, group-membership) registration: unlike
 * xtc_reg_register, many pids may share one key.  This is the substrate
 * for process groups (xtc_pg).  Registering the same (key, pid) twice is
 * idempotent (returns XTC_OK without adding a second node).
 */
int
xtc_reg_register_dup(xtc_reg_t *r, const char *key, xtc_pid_t pid)
{
	struct reg_node *node;
	uint32_t h, b;
	int rc = XTC_OK;
	if (r == NULL || key == NULL) return XTC_E_INVAL;
	h = __reg_hash(key);
	b = h & (REG_NBUCKETS - 1);
	(void)__xtc_mtx_lock(&r->lock);
	/* Idempotent: skip if this exact (key, pid) is already present. */
	for (node = r->buckets[b]; node != NULL; node = node->next)
		if (node->hash == h && node->dup &&
		    strcmp(node->name, key) == 0 &&
		    xtc_pid_eq(node->pid, pid)) {
			goto out;   /* already a member */
		}
	if ((rc = __os_calloc(1, sizeof *node, (void **)&node)) != XTC_OK)
		goto out;
	if ((rc = __os_strdup(key, &node->name)) != XTC_OK) {
		__os_free(node);
		goto out;
	}
	node->hash = h;
	node->pid = pid;
	node->dup = 1;
	node->next = r->buckets[b];
	r->buckets[b] = node;
	r->n++;
out:
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

/*
 * Remove one (key, pid) duplicate-key entry (a group leave).  Returns
 * XTC_OK if removed, XTC_E_INVAL if not a member.
 */
int
xtc_reg_unregister_pid(xtc_reg_t *r, const char *key, xtc_pid_t pid)
{
	struct reg_node **link, *node;
	uint32_t h;
	int rc = XTC_E_INVAL;
	if (r == NULL || key == NULL) return XTC_E_INVAL;
	h = __reg_hash(key);
	(void)__xtc_mtx_lock(&r->lock);
	for (link = &r->buckets[h & (REG_NBUCKETS - 1)];
	    (node = *link) != NULL; link = &node->next) {
		if (node->hash == h && strcmp(node->name, key) == 0 &&
		    xtc_pid_eq(node->pid, pid)) {
			*link = node->next;
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

/*
 * Remove `pid` from every key it appears under -- the "process left
 * everything" cleanup.  O(buckets + entries); called once when a
 * process exits or a connection closes.  Returns the count removed.
 */
int
xtc_reg_drop_pid(xtc_reg_t *r, xtc_pid_t pid)
{
	uint32_t b;
	int removed = 0;
	if (r == NULL) return 0;
	(void)__xtc_mtx_lock(&r->lock);
	for (b = 0; b < REG_NBUCKETS; b++) {
		struct reg_node **link = &r->buckets[b], *node;
		while ((node = *link) != NULL) {
			if (xtc_pid_eq(node->pid, pid)) {
				*link = node->next;
				__os_free(node->name);
				__os_free(node);
				r->n--;
				removed++;
				/* do not advance link: it now points at
				 * the next node */
			} else {
				link = &node->next;
			}
		}
	}
	(void)__xtc_mtx_unlock(&r->lock);
	return removed;
}

/*
 * Visit every pid registered under `key` (both the unique entry and all
 * duplicate-key members).  The callback runs UNDER the registry lock, so
 * it must be brief and must not call back into the registry; copy pids
 * out if more work is needed.  A nonzero callback return stops the walk.
 * Returns the number of members visited.
 */
int
xtc_reg_members(xtc_reg_t *r, const char *key,
                int (*fn)(xtc_pid_t pid, void *user), void *user)
{
	struct reg_node *node;
	uint32_t h;
	int count = 0;
	if (r == NULL || key == NULL || fn == NULL) return 0;
	h = __reg_hash(key);
	(void)__xtc_mtx_lock(&r->lock);
	for (node = r->buckets[h & (REG_NBUCKETS - 1)]; node != NULL;
	    node = node->next) {
		if (node->hash == h && strcmp(node->name, key) == 0) {
			count++;
			if (fn(node->pid, user) != 0)
				break;
		}
	}
	(void)__xtc_mtx_unlock(&r->lock);
	return count;
}

/* ===== crash-aware registry: reaper proc + monitored registration ===== */

/*
 * The reaper is a small proc the embedder spawns once with
 * xtc_reg_reaper as the body and the registry as the argument.  It
 * registers its own pid in the registry (so xtc_reg_register_mon can
 * find it), then loops: each REG_REAPER_MONITOR request arms an
 * xtc_monitor on the named pid; every DOWN it receives (a monitored
 * pid exited) triggers xtc_reg_drop_pid so the dead pid leaves every
 * key it held.  This is the automatic form of the manual
 * xtc_reg_drop_pid cleanup -- the crash-aware registry.
 */
void
xtc_reg_reaper(void *arg)
{
	struct xtc_reg *r = arg;
	if (r == NULL) return;

	/* Publish the reaper's pid so xtc_reg_register_mon can reach it. */
	(void)__xtc_mtx_lock(&r->lock);
	r->reaper = xtc_self();
	atomic_store_explicit(&r->has_reaper, 1, memory_order_release);
	(void)__xtc_mtx_unlock(&r->lock);

	for (;;) {
		void *msg = NULL;
		size_t len = 0;
		xtc_pid_t down_pid;
		int reason;

		if (xtc_recv(&msg, &len, -1) != XTC_OK)
			break;               /* loop torn down */
		if (msg == NULL)
			continue;

		/* A monitor request from xtc_reg_register_mon. */
		if (len == sizeof(struct reg_reaper_msg)) {
			struct reg_reaper_msg m;
			memcpy(&m, msg, sizeof m);
			if (m.tag == REG_REAPER_MONITOR) {
				/* Monitor the pid; a DOWN (immediate if it is
				 * already gone) comes back to us. */
				(void)xtc_monitor(m.pid, NULL);
				__os_free(msg);
				continue;
			}
		}

		/* Otherwise it should be a DOWN for a pid we monitor. */
		if (xtc_down_decode(msg, len, &down_pid, &reason) == XTC_OK) {
			(void)reason;
			(void)xtc_reg_drop_pid(r, down_pid);
		}
		__os_free(msg);
	}

	/* Reaper exiting: clear the published pid so later _register_mon
	 * calls fall back to a plain register rather than sending into a
	 * dead mailbox. */
	(void)__xtc_mtx_lock(&r->lock);
	atomic_store_explicit(&r->has_reaper, 0, memory_order_release);
	(void)__xtc_mtx_unlock(&r->lock);
}

int
xtc_reg_register_mon(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	int rc;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	if ((rc = xtc_reg_register(r, name, pid)) != XTC_OK)
		return rc;
	/* If a reaper is running, ask it to monitor pid so the entry is
	 * auto-dropped on DOWN.  If none, this behaves as a plain register
	 * (the caller may still xtc_reg_drop_pid manually). */
	if (atomic_load_explicit(&r->has_reaper, memory_order_acquire)) {
		struct reg_reaper_msg m;
		xtc_pid_t reaper;
		(void)__xtc_mtx_lock(&r->lock);
		reaper = r->reaper;
		(void)__xtc_mtx_unlock(&r->lock);
		m.tag = REG_REAPER_MONITOR;
		m.pid = pid;
		(void)xtc_send(reaper, &m, sizeof m);
	}
	return XTC_OK;
}

int
xtc_svr_call_name(xtc_reg_t *r, const char *name,
                  const void *req, size_t req_size,
                  void **out_reply, size_t *out_size, int64_t timeout_ns)
{
	xtc_pid_t pid;
	int rc;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	if ((rc = xtc_reg_whereis(r, name, &pid)) != XTC_OK)
		return XTC_E_NOTFOUND;
	return xtc_svr_call(pid, req, req_size, out_reply, out_size,
	    timeout_ns);
}
