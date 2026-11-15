/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/reg.c
 *	Process registry implementation.  M10.5: a chained FNV-1a hash
 *	table (name -> xtc_pid_t) under a single mutex.  Lookup,
 *	register, and unregister are O(1) average: the bucket array
 *	doubles whenever the entry count passes the bucket count.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_reg.h"
#include "xtc_proc.h"      /* reaper proc: monitor + recv + drop-on-DOWN */
#include "xtc_svr.h"       /* xtc_svr_call_name via-dispatch */
#include "xtc_app.h"       /* xtc_app_is_shutdown_msg: the reaper drains */
#include "xtc_sync.h"      /* xtc_notify: xtc_reg_whereis_wait */
#include "proc_int.h"      /* __xtc_proc_alive */
#include "xtc_sim.h"       /* XTC_SIM_BUGGIFY / xtc_sim_fault (DST) */
#include "xtc_dst_inject.h" /* DST bug-injection harness (no-op in prod) */
#include "xtc_tail.h"      /* XTC_TAIL_LIFECYCLE_DROP kind */
#include "tail_int.h"      /* __xtc_tail_emit */

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/*
 * Initial power-of-two bucket count.  The table doubles (under the lock,
 * rehashing in place) when an insert takes the entry count past the
 * bucket count, so chains stay O(1) at any size.  It never shrinks.
 * Before 1.55.1 the 256 buckets were fixed: whereis cost 47 ns at 100
 * names and 5.1 us at 100k.
 */
#define REG_NBUCKETS0 256u              /* power of two */

struct reg_node {
	struct reg_node *next;          /* collision chain link */
	uint32_t         hash;          /* cached FNV-1a, fast chain reject */
	xtc_pid_t        pid;
	char            *name;          /* __os_strdup'd, owned by node */
	int              dup;           /* 1 = duplicate-key (pub/sub) entry */
	void            *value;         /* xtc_reg_put's caller-owned slot */
};

/* An xtc_reg_whereis_wait caller.  Heap-allocated and linked on the
 * registry under its lock; every insert of an entry whose name hashes to
 * `hash` signals it, and it re-checks. */
struct reg_waiter {
	struct reg_waiter *next;
	uint32_t hash;
	xtc_notify_t *wake;
	struct xtc_reg *reg; /* for the kill-path unlink */
};

struct xtc_reg {
	pthread_mutex_t   lock;
	struct reg_node **buckets;      /* nbuckets heads */
	uint32_t          nbuckets;     /* power of two */
	int               n;            /* live entry count */
	int has_reaper; /* 1 once a reaper proc has registered */
	xtc_pid_t         reaper;       /* the reaper's pid (valid if has_reaper) */
	struct reg_waiter *waiters;     /* xtc_reg_whereis_wait callers */
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
	rc = __os_calloc(REG_NBUCKETS0, sizeof *r->buckets,
	    (void **)&r->buckets);
	if (rc != XTC_OK) {
		__os_free(r);
		return rc;
	}
	r->nbuckets = REG_NBUCKETS0;
	(void)pthread_mutex_init(&r->lock, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_reg_destroy(xtc_reg_t *r)
{
	uint32_t b;
	if (r == NULL) return;
	for (b = 0; b < r->nbuckets; b++) {
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

/* Bucket head for hash h.  Called with r->lock held. */
static struct reg_node **
__reg_bucket(struct xtc_reg *r, uint32_t h)
{
	return &r->buckets[h & (r->nbuckets - 1)];
}

/* Wake every xtc_reg_whereis_wait caller waiting on a name with hash h
 * (a spurious wake for a colliding name only makes it re-check).  Called
 * with r->lock held; xtc_notify_signal takes only the notify's own lock.
 * Planted bug 46 (REG_WAIT_NOWAKE) skips it: a waiter then sees a new
 * registration only when its timeout expires. */
static void
__reg_wake_locked(struct xtc_reg *r, uint32_t h)
{
	struct reg_waiter *w;
#if !XTC_DST_BUG(XTC_DST_BUG_REG_WAIT_NOWAKE)
	for (w = r->waiters; w != NULL; w = w->next)
		if (w->hash == h)
			(void)xtc_notify_signal(w->wake);
#else
	(void)r;
	(void)h;
	(void)w;
#endif
}

/*
 * Insert a fresh node (head-insert), doubling the bucket array first if
 * the table is at load factor 1.  Called with r->lock held.  A failed
 * grow is not an error: the insert still succeeds, chains just get
 * longer until a later grow succeeds.
 */
static void
__reg_insert_locked(struct xtc_reg *r, struct reg_node *node)
{
	struct reg_node **nb, **head, *e, *next;
	uint32_t nn, b;

	if ((uint32_t)r->n >= r->nbuckets && r->nbuckets <= UINT32_MAX / 2 &&
	    __os_calloc((size_t)r->nbuckets * 2, sizeof *nb,
	    (void **)&nb) == XTC_OK) {
		nn = r->nbuckets * 2;
		for (b = 0; b < r->nbuckets; b++)
			for (e = r->buckets[b]; e != NULL; e = next) {
				next = e->next;
				e->next = nb[e->hash & (nn - 1)];
				nb[e->hash & (nn - 1)] = e;
			}
		__os_free(r->buckets);
		r->buckets = nb;
		r->nbuckets = nn;
	}
	head = __reg_bucket(r, node->hash);
	node->next = *head;
	*head = node;
	r->n++;
	__reg_wake_locked(r, node->hash);
}

/* Find a node by name in its bucket.  Called with r->lock held. */
static struct reg_node *
__reg_find_locked(struct xtc_reg *r, const char *name, uint32_t h)
{
	struct reg_node *node;
	for (node = *__reg_bucket(r, h); node != NULL;
	    node = node->next)
		if (node->hash == h && strcmp(node->name, name) == 0)
			return node;
	return NULL;
}

int
xtc_reg_register(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	struct reg_node *node;
	uint32_t h;
	int rc = XTC_OK;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	h = __reg_hash(name);
	(void)__xtc_mtx_lock(&r->lock);
#if XTC_DST_BUG(XTC_DST_BUG_REGDUP)
	/* planted bug (REGDUP): skip the already-registered check -> a
	 * second pid can register a name a first still holds, so two winners
	 * hold one name at once and test_sim_reg's at-most-one-holder
	 * invariant fires (g_bad_dup). */
#else
	if (__reg_find_locked(r, name, h) != NULL) { rc = XTC_E_INVAL; goto out; }
#endif
	if ((rc = __os_calloc(1, sizeof *node, (void **)&node)) != XTC_OK)
		goto out;
	if ((rc = __os_strdup(name, &node->name)) != XTC_OK) {
		__os_free(node);
		goto out;
	}
	node->hash = h;
	node->pid = pid;
	__reg_insert_locked(r, node);
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
	for (link = __reg_bucket(r, h); (node = *link) != NULL;
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
	uint32_t h;
	int rc = XTC_OK;
	if (r == NULL || key == NULL) return XTC_E_INVAL;
	h = __reg_hash(key);
	(void)__xtc_mtx_lock(&r->lock);
	/* Idempotent: skip if this exact (key, pid) is already present. */
	for (node = *__reg_bucket(r, h); node != NULL; node = node->next)
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
	__reg_insert_locked(r, node);
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
	for (link = __reg_bucket(r, h);
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
	for (b = 0; b < r->nbuckets; b++) {
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
	for (node = *__reg_bucket(r, h); node != NULL;
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
	xtc_pid_t self = xtc_self();
	if (r == NULL) return;
	/* Killed before it first ran (xtc_app_start that failed after
	 * spawning it): the registry may be gone, so do not touch it. */
	if (xtc_cancel_requested())
		return;

	/* Publish the reaper's pid so xtc_reg_register_mon can reach it.  A
	 * second reaper for the same registry takes over enrollment. */
	(void)__xtc_mtx_lock(&r->lock);
	r->reaper = self;
	xtc_atomic_store_i32(&r->has_reaper, 1, xtc_mo_release);
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
		/* Run as an app's supervised child, it drains like one. */
		if (xtc_app_is_shutdown_msg(msg, len)) {
			__os_free(msg);
			break;
		}

		/* A monitor request from xtc_reg_register_mon. */
		if (len == sizeof(struct reg_reaper_msg)) {
			struct reg_reaper_msg m;
			memcpy(&m, msg, sizeof m);
			if (m.tag == REG_REAPER_MONITOR) {
				/* Monitor the pid; a DOWN (immediate if it is
				 * already gone) comes back to us.
				 *
				 * xtc_monitor of an already-dead pid still
				 * returns OK and synthesizes a DOWN, so its
				 * only failure is allocation.  If it fails, NO
				 * DOWN will ever arrive for this pid and its
				 * names would outlive it forever -- so drop
				 * them now rather than leave a stale entry.
				 * (The previous code ignored this rc.) */
				if (xtc_monitor(m.pid, NULL) != XTC_OK) {
					__xtc_tail_emit(XTC_TAIL_SCHED,
					    XTC_TAIL_LIFECYCLE_DROP, m.pid, 0);
					(void)xtc_reg_drop_pid(r, m.pid);
				}
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
	 * dead mailbox -- unless another reaper has taken over since. */
	(void)__xtc_mtx_lock(&r->lock);
	if (xtc_pid_eq(r->reaper, self))
		xtc_atomic_store_i32(&r->has_reaper, 0, xtc_mo_release);
	(void)__xtc_mtx_unlock(&r->lock);
}

/*
 * Publish (or, with XTC_PID_NONE, withdraw) a reaper pid before the
 * reaper first runs, so an enrollment sent meanwhile waits in its
 * mailbox.  xtc_app_start uses it for the app's own reaper.
 *
 * PUBLIC: void __xtc_reg_reaper_set __P((xtc_reg_t *, xtc_pid_t));
 */
void
__xtc_reg_reaper_set(xtc_reg_t *r, xtc_pid_t pid)
{
	(void)__xtc_mtx_lock(&r->lock);
	r->reaper = pid;
	xtc_atomic_store_i32(&r->has_reaper, !xtc_pid_is_none(pid),
	    xtc_mo_release);
	(void)__xtc_mtx_unlock(&r->lock);
}

/*
 * Ask the reaper (if one runs) to monitor `pid` so the entry `name` is
 * dropped when pid goes DOWN; on a failed enrollment roll the entry back
 * and report it.  Shared by xtc_reg_register_mon and xtc_reg_put with
 * XTC_REG_MONITOR.  The entry is already in the table.
 */
static int
__reg_enroll_mon(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	int rc;
	/* If a reaper is running, ask it to monitor pid so the entry is
	 * auto-dropped on DOWN.  If none, this behaves as a plain register
	 * (the caller may still xtc_reg_drop_pid manually). */
	if (xtc_atomic_load_i32(&r->has_reaper, xtc_mo_acquire)) {
		struct reg_reaper_msg m;
		xtc_pid_t reaper;
		(void)__xtc_mtx_lock(&r->lock);
		reaper = r->reaper;
		(void)__xtc_mtx_unlock(&r->lock);
		m.tag = REG_REAPER_MONITOR;
		m.pid = pid;
		/* Enrollment is what makes this a MONITORED registration: if
		 * the reaper never receives it, the name will not be removed
		 * when the pid dies.  Returning XTC_OK then would promise
		 * automatic cleanup with no coverage behind it -- measured: 3
		 * registrations all returned OK and 2 names outlived their pids.
		 *
		 * The send is bounded (the reaper's mailbox can be full), so
		 * the loss is real and possible.  Roll the registration BACK
		 * and report it, so the caller never holds a "monitored" name
		 * that nothing is watching.  xtc_reg_unregister_pid removes
		 * the name only if it is still bound to THIS pid, so it cannot
		 * clobber a concurrent re-registration of the same name.  The
		 * drop stays diagnosable via XTC_TAIL_LIFECYCLE_DROP too.
		 *
		 * CONTRACT CHANGE (1.50): this used to return XTC_OK here. */
		if ((rc = xtc_send(reaper, &m, sizeof m)) != XTC_OK) {
			__xtc_tail_emit(XTC_TAIL_SCHED,
			    XTC_TAIL_LIFECYCLE_DROP, pid,
			    (uint64_t)(int64_t)rc);
			(void)xtc_reg_unregister_pid(r, name, pid);
			return (rc == XTC_E_RESOURCE) ? XTC_E_RESOURCE
			                              : XTC_E_AGAIN;
		}
	}
	return XTC_OK;
}

int
xtc_reg_register_mon(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	int rc;
	if (r == NULL || name == NULL) return XTC_E_INVAL;
	if ((rc = xtc_reg_register(r, name, pid)) != XTC_OK)
		return rc;
	return __reg_enroll_mon(r, name, pid);
}

/*
 * PUBLIC: int       xtc_reg_put __P((xtc_reg_t *, const char *, xtc_pid_t, void *, unsigned));
 *
 * One entry: name -> (pid, value), the general form of register /
 * register_dup / register_mon.  A unique name that is taken is
 * XTC_E_EXIST (xtc_reg_register says XTC_E_INVAL for the same thing; it
 * keeps that for compatibility).  XTC_REG_DUP adds a duplicate-key
 * member; putting the same (key, pid) twice is idempotent and keeps the
 * first value.  XTC_REG_MONITOR enrolls pid with the reaper, as
 * xtc_reg_register_mon does, rolling the entry back if that fails.
 */
int
xtc_reg_put(xtc_reg_t *r, const char *name, xtc_pid_t pid, void *value,
            unsigned flags)
{
	struct reg_node *node;
	uint32_t h;
	int dup, rc = XTC_OK;

	if (r == NULL || name == NULL ||
	    (flags & ~(XTC_REG_DUP | XTC_REG_MONITOR)) != 0)
		return XTC_E_INVAL;
	dup = (flags & XTC_REG_DUP) != 0;
	h = __reg_hash(name);
	(void)__xtc_mtx_lock(&r->lock);
	/* Unique: any entry under the name blocks it.  Duplicate-key: only
	 * the same (key, pid) is a no-op; a unique holder of the key does
	 * not block group members, as with xtc_reg_register_dup. */
	for (node = *__reg_bucket(r, h); node != NULL; node = node->next) {
		if (node->hash != h || strcmp(node->name, name) != 0)
			continue;
		if (!dup) {
#if !XTC_DST_BUG(XTC_DST_BUG_REGDUP)
			rc = XTC_E_EXIST;
			goto out;
#endif
		} else if (node->dup && xtc_pid_eq(node->pid, pid)) {
			goto out;   /* already a member */
		}
	}
	if ((rc = __os_calloc(1, sizeof *node, (void **)&node)) != XTC_OK)
		goto out;
	if ((rc = __os_strdup(name, &node->name)) != XTC_OK) {
		__os_free(node);
		goto out;
	}
	node->hash = h;
	node->pid = pid;
	node->dup = dup;
	node->value = value;
	__reg_insert_locked(r, node);
out:
	(void)__xtc_mtx_unlock(&r->lock);
	if (rc == XTC_OK && (flags & XTC_REG_MONITOR) != 0)
		rc = __reg_enroll_mon(r, name, pid);
	return rc;
}

/*
 * PUBLIC: int       xtc_reg_visit __P((xtc_reg_t *, const char *, xtc_reg_visit_fn, void *, int *));
 *
 * Call fn(pid, value, user) for each entry under `name`, holding the
 * registry lock for the whole walk.  That is what makes the value safe to
 * use: an owner that removes its entry (unregister / drop_pid) before it
 * frees the value cannot free it while fn runs, because the removal waits
 * for this lock.  So fn must be brief, must not park, and must not call
 * into this registry (it would deadlock); xtc_proc_wake and atomic stores
 * are fine.  A nonzero return from fn stops the walk.
 *
 * A miss is authoritative: XTC_E_NOTFOUND means no entry existed at the
 * moment of the walk (unlike xtc_reg_whereis there is no simulated
 * transient miss).  *out_n (optional) gets the number of entries visited.
 */
int
xtc_reg_visit(xtc_reg_t *r, const char *name, xtc_reg_visit_fn fn,
              void *user, int *out_n)
{
	struct reg_node *node;
	uint32_t h;
	int n = 0;

	if (out_n != NULL)
		*out_n = 0;
	if (r == NULL || name == NULL || fn == NULL)
		return XTC_E_INVAL;
	h = __reg_hash(name);
	(void)__xtc_mtx_lock(&r->lock);
	for (node = *__reg_bucket(r, h); node != NULL; node = node->next) {
		if (node->hash != h || strcmp(node->name, name) != 0)
			continue;
		n++;
		if (fn(node->pid, node->value, user) != 0)
			break;
	}
	(void)__xtc_mtx_unlock(&r->lock);
	if (out_n != NULL)
		*out_n = n;
	return n > 0 ? XTC_OK : XTC_E_NOTFOUND;
}

/*
 * Replace-or-insert name -> pid as a unique entry, for the supervisor's
 * child names: an entry under `name` (unique, of any earlier incarnation)
 * is overwritten in place, so the name is never absent between two
 * incarnations and never names two.  Returns XTC_OK or XTC_E_NOMEM.
 *
 * PUBLIC: int __xtc_reg_set __P((xtc_reg_t *, const char *, xtc_pid_t));
 */
int
__xtc_reg_set(xtc_reg_t *r, const char *name, xtc_pid_t pid)
{
	struct reg_node *node;
	uint32_t h;
	int rc = XTC_OK;
	h = __reg_hash(name);
	(void)__xtc_mtx_lock(&r->lock);
	for (node = *__reg_bucket(r, h); node != NULL; node = node->next)
		if (node->hash == h && !node->dup &&
		    strcmp(node->name, name) == 0)
			break;
	if (node != NULL) {
		node->pid = pid;
		node->value = NULL;
		__reg_wake_locked(r, h);
		goto out;
	}
	if ((rc = __os_calloc(1, sizeof *node, (void **)&node)) != XTC_OK)
		goto out;
	if ((rc = __os_strdup(name, &node->name)) != XTC_OK) {
		__os_free(node);
		goto out;
	}
	node->hash = h;
	node->pid = pid;
	__reg_insert_locked(r, node);
out:
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

/* The pid of an entry under `name` that is alive and not `not_pid`, if
 * any.  Called with r->lock held. */
static int
__reg_pick_live_locked(struct xtc_reg *r, const char *name, uint32_t h,
    xtc_pid_t not_pid, xtc_pid_t *out)
{
	struct reg_node *node;
	for (node = *__reg_bucket(r, h); node != NULL; node = node->next)
		if (node->hash == h && strcmp(node->name, name) == 0 &&
		    !xtc_pid_eq(node->pid, not_pid) &&
		    __xtc_proc_alive(node->pid)) {
			*out = node->pid;
			return 1;
		}
	return 0;
}

/* Unlink and free a waiter: the normal return of xtc_reg_whereis_wait,
 * and its scope finalizer when the waiting proc is killed or faults.
 * NB: the argument is the heap waiter itself, never a stack struct of
 * the waiting frame -- a kill runs the finalizer after unwinding it. */
static void
__reg_waiter_drop(void *arg)
{
	struct reg_waiter *w = arg, **pp;
	struct xtc_reg *r = w->reg;
	(void)__xtc_mtx_lock(&r->lock);
	for (pp = &r->waiters; *pp != NULL; pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			break;
		}
	(void)__xtc_mtx_unlock(&r->lock);
	xtc_notify_destroy(w->wake);
	__os_free(w);
}

/*
 * PUBLIC: int xtc_reg_whereis_wait __P((xtc_reg_t *, const char *, xtc_pid_t, int64_t, xtc_pid_t *));
 *
 * Park until `name` names a live pid other than `not_pid`, woken by the
 * registration (an insert or a replacement of the name), not by a
 * timer.  The common use: a client whose server died waits for the
 * supervisor to register the next incarnation (not_pid = the dead one).
 * The lookup is exact (no simulated transient miss, unlike
 * xtc_reg_whereis).  From a proc it parks the fiber; from a plain thread
 * it blocks the thread.  Kill-safe: a killed waiter is unlinked by a
 * scope finalizer before its wait state is freed.
 */
int
xtc_reg_whereis_wait(xtc_reg_t *r, const char *name, xtc_pid_t not_pid,
    int64_t timeout_ns, xtc_pid_t *out_pid)
{
	struct reg_waiter *w = NULL;
	xtc_scope_t *sc = NULL;
	uint32_t h;
	int64_t deadline = 0, now = 0;
	int rc;

	if (r == NULL || name == NULL || out_pid == NULL)
		return XTC_E_INVAL;
	h = __reg_hash(name);
	(void)__xtc_mtx_lock(&r->lock);
	if (__reg_pick_live_locked(r, name, h, not_pid, out_pid)) {
		(void)__xtc_mtx_unlock(&r->lock);
		return XTC_OK;
	}
	(void)__xtc_mtx_unlock(&r->lock);
	if (timeout_ns == 0)
		return XTC_E_AGAIN;

	/* Waiter state on the heap, linked under the lock it is checked
	 * under, so an insert between the miss above and the park below
	 * still signals it (the notify stores one signal). */
	if ((rc = __os_calloc(1, sizeof *w, (void **)&w)) != XTC_OK)
		return rc;
	if ((rc = xtc_notify_create(&w->wake)) != XTC_OK) {
		__os_free(w);
		return rc;
	}
	w->hash = h;
	w->reg = r;
	(void)__xtc_mtx_lock(&r->lock);
	w->next = r->waiters;
	r->waiters = w;
	(void)__xtc_mtx_unlock(&r->lock);
	if (!xtc_pid_is_none(xtc_self())) {
		if ((sc = xtc_scope_open()) == NULL) {
			__reg_waiter_drop(w);
			return XTC_E_RESOURCE;
		}
		if (xtc_scope_defer(sc, __reg_waiter_drop, w) != XTC_OK) {
			xtc_scope_close(sc);
			__reg_waiter_drop(w);
			return XTC_E_RESOURCE;
		}
	}

	if (timeout_ns > 0) {
		(void)__os_clock_mono(&deadline);
		deadline = timeout_ns > INT64_MAX - deadline ?
		    INT64_MAX :
		    deadline + timeout_ns;
	}
	for (;;) {
		int64_t wait_ns = -1;
		(void)__xtc_mtx_lock(&r->lock);
		rc = __reg_pick_live_locked(r, name, h, not_pid, out_pid) ?
		    XTC_OK :
		    XTC_E_AGAIN;
		(void)__xtc_mtx_unlock(&r->lock);
		if (rc == XTC_OK)
			break;
		if (timeout_ns > 0) {
			(void)__os_clock_mono(&now);
			if (now >= deadline)
				break;
			wait_ns = deadline - now;
		}
		(void)xtc_notify_wait(w->wake, wait_ns);
	}
	if (sc != NULL)
		xtc_scope_close(sc); /* runs __reg_waiter_drop */
	else
		__reg_waiter_drop(w);
	return rc;
}

/*
 * PUBLIC: int       xtc_reg_send_wait __P((xtc_reg_t *, const char *, const void *, size_t, int64_t));
 *
 * Send to whatever live proc `name` names, waiting while it is
 * unregistered (a supervised child between incarnations) and while its
 * mailbox is full: xtc_reg_whereis_wait, then xtc_send_wait.  A holder
 * that dies before or during the send (XTC_E_INVAL) is excluded and the
 * next one waited for.  XTC_OK means the message is in a live
 * incarnation's mailbox; if that incarnation dies before taking it, the
 * message is lost with its mailbox, as any queued message is.  One
 * deadline for the whole call.  From a proc it parks the fiber; from a
 * plain thread it blocks it.
 */
int
xtc_reg_send_wait(xtc_reg_t *r, const char *name, const void *data,
                  size_t size, int64_t timeout_ns)
{
	xtc_pid_t dead = XTC_PID_NONE, to;
	int64_t deadline = 0, now = 0, left = timeout_ns;
	int rc;

	if (r == NULL || name == NULL || (size > 0 && data == NULL))
		return XTC_E_INVAL;
	if (timeout_ns > 0) {
		(void)__os_clock_mono(&deadline);
		deadline = timeout_ns > INT64_MAX - deadline ? INT64_MAX :
		    deadline + timeout_ns;
	}
	for (;;) {
		if (timeout_ns > 0) {
			(void)__os_clock_mono(&now);
			if (now >= deadline)
				return XTC_E_AGAIN;
			left = deadline - now;
		}
		if ((rc = xtc_reg_whereis_wait(r, name, dead, left, &to)) !=
		    XTC_OK)
			return rc;
		rc = xtc_send_wait(to, data, size, left);
		if (rc != XTC_E_INVAL)
			return rc;
		dead = to;      /* died under us: wait for the next one */
	}
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
