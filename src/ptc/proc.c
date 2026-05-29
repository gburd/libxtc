/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/proc.c
 *	BEAM-style processes built on top of the M4 coroutine substrate.
 *	Each process is a fiber with identity, a mailbox, and signal
 *	handling for links and monitors.  Selective receive uses the
 *	classic save-queue pattern.
 */

#include "xtc_int.h"
#include "xtc_inject.h"
#include "xtc_proc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "loop_int.h"
#include "coro_int.h"
#include "xtc_tailcall.h"
#include "xtc_slab.h"
#include "xtc_slab.h"
#include <stdio.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---------- envelope ---------- */

struct envelope {
	struct envelope *next;
	xtc_pid_t        from;
	size_t           size;
	unsigned char    data[];     /* flexible */
};

/* ---------- proc ---------- */

struct link_entry { struct link_entry *next; xtc_pid_t peer; };
struct mon_entry  { struct mon_entry *next; uint64_t ref; xtc_pid_t target; xtc_pid_t watcher; };

/* M11.5b: pools for link_entry / mon_entry (fixed-size, hot path). */
static xtc_slab_t      *__link_slab     = NULL;
static xtc_slab_t      *__mon_slab      = NULL;
static xtc_slab_t      *__env_slab      = NULL;
static pthread_mutex_t  __proc_slab_init_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Small-message envelope pool.  Message passing is the actor hot
 * path; allocating every envelope with malloc puts an allocator
 * round-trip on every send.  Envelopes whose payload fits in
 * ENV_POOL_PAYLOAD bytes are served from a fixed-size slab instead
 * (one magazine pop on the fast path, no global allocator call);
 * larger payloads fall back to malloc.  The discriminator on free is
 * the envelope's own size field -- a pooled envelope always has
 * size <= ENV_POOL_PAYLOAD -- so no per-envelope flag is needed.
 */
#define ENV_POOL_PAYLOAD  256

static void
__proc_slabs_ensure(void)
{
	if (__link_slab != NULL && __mon_slab != NULL && __env_slab != NULL)
		return;
	(void)pthread_mutex_lock(&__proc_slab_init_lock);
	if (__link_slab == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		o.name = "proc.link"; o.obj_size = sizeof(struct link_entry);
		(void)xtc_slab_create(&o, &__link_slab);
	}
	if (__mon_slab == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		o.name = "proc.mon"; o.obj_size = sizeof(struct mon_entry);
		(void)xtc_slab_create(&o, &__mon_slab);
	}
	if (__env_slab == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		o.name = "proc.env";
		o.obj_size = sizeof(struct envelope) + ENV_POOL_PAYLOAD;
		(void)xtc_slab_create(&o, &__env_slab);
	}
	(void)pthread_mutex_unlock(&__proc_slab_init_lock);
}

/* Allocate an envelope for a `size`-byte payload, from the pool when
 * it fits, else from malloc. */
static struct envelope *
__env_alloc(size_t size)
{
	if (size <= ENV_POOL_PAYLOAD) {
		__proc_slabs_ensure();
		if (XTC_LIKELY(__env_slab != NULL))
			return xtc_slab_alloc(__env_slab);
	}
	return malloc(sizeof(struct envelope) + size);
}

/* Free an envelope, routing by its payload size (the same predicate
 * __env_alloc used). */
static void
__env_free(struct envelope *e)
{
	if (e == NULL) return;
	if (e->size <= ENV_POOL_PAYLOAD && __env_slab != NULL)
		xtc_slab_free(__env_slab, e);
	else
		free(e);
}

static struct link_entry *
__link_alloc(void)
{
	__proc_slabs_ensure();
	if (__link_slab == NULL) return NULL;
	return xtc_slab_alloc(__link_slab);
}
static void __link_free(struct link_entry *e) {
	if (e == NULL) return;
	if (__link_slab) xtc_slab_free(__link_slab, e); else free(e);
}
static struct mon_entry *
__mon_alloc(void)
{
	__proc_slabs_ensure();
	if (__mon_slab == NULL) return NULL;
	return xtc_slab_alloc(__mon_slab);
}
static void __mon_free(struct mon_entry *e) {
	if (e == NULL) return;
	if (__mon_slab) xtc_slab_free(__mon_slab, e); else free(e);
}

struct xtc_proc {
	xtc_pid_t   pid;
	xtc_loop_t *loop;
	xtc_task_t *task;            /* the underlying task */
	struct xtc_coro *coro;        /* the underlying fiber */

	/*
	 * Mailbox (singly-linked FIFO of envelopes).
	 *
	 * False-sharing fix: the producer-side fields (mbox_lock,
	 * mbox_tail) are written by senders on remote threads, while
	 * the consumer-side fields (mbox_head, mbox_n) are written
	 * by the owning proc.  We cache-line-separate them to avoid
	 * cache-line ping-pong under concurrent send/recv.
	 *
	 * Layout:
	 *   [consumer-side] mbox_head, mbox_n
	 *   <64-byte pad>
	 *   [producer-side] mbox_lock, mbox_tail, mbox_cap
	 */
	/* ---- consumer-side (owner writes) ---- */
	struct envelope  *mbox_head;
	size_t            mbox_n;

	char              __mbox_pad[XTC_CACHE_LINE
	                            - sizeof(struct envelope *)
	                            - sizeof(size_t)];

	/* ---- producer-side (senders write) ---- */
	pthread_mutex_t   mbox_lock;
	struct envelope  *mbox_tail;
	size_t            mbox_cap;

	/* Selective-receive save queue (envelopes that the receiver
	 * already inspected and rejected). */
	struct envelope  *save_head;
	struct envelope  *save_tail;

	/* BEAM recv-mark optimization for selective receive.  When the
	 * caller invokes xtc_recv_match repeatedly with the same
	 * (match_fn, user) pair, we remember which save_queue entries
	 * have already been tested against that predicate so we don't
	 * re-walk them.  Cleared whenever the predicate changes. */
	xtc_match_fn      last_match_fn;
	void             *last_match_user;
	struct envelope  *recv_mark;       /* skip up to and including this entry */

	/* Receive coordination. */
	xtc_waker_t       recv_waker;
	int               waker_armed;

	/* Entry. */
	xtc_proc_fn fn;
	void       *arg;

	/* Exit handling. */
	jmp_buf     exit_jb;
	int         exit_jb_set;
	int         exit_reason;

	/* Asynchronous kill (cross-process exit signal).  Set by
	 * xtc_exit_pid; checked at every yield/recv parking point.
	 * The flag carries the reason+1 so 0 means "no kill pending". */
	_Atomic int kill_pending;

	/* Lifecycle. */
	int         alive;

	/* Links & monitors. */
	struct link_entry *links;
	struct mon_entry  *monitors;     /* monitors WE created (we are watcher) */
	struct mon_entry  *monitored_by; /* monitors others created on us */
};

/*
 * Static assertion: mbox producer/consumer fields are cache-line separated.
 * offsetof(mbox_lock) should be at least 64 bytes beyond offsetof(mbox_head).
 */
_Static_assert(
    offsetof(struct xtc_proc, mbox_lock) - offsetof(struct xtc_proc, mbox_head)
        >= XTC_CACHE_LINE,
    "mbox producer/consumer fields must be cache-line separated");

/*
 * Per-loop slot table.  A loop owns its own proc_slots array; we
 * grow on demand (powers of two).  Slot reuse bumps the generation
 * so stale pids can be detected.
 */
struct xtc_proc_slot {
	struct xtc_proc *proc;       /* NULL if free */
	uint32_t         gen;
};
struct xtc_proc_table {
	struct xtc_proc_slot *slots;
	size_t                cap;
	size_t                n_used;
	pthread_mutex_t       lock;
	int                   inited;
};

/* Each loop carries one of these (lazy-allocated on first proc spawn). */
static XTC_THREAD_LOCAL struct xtc_proc *__current_proc = NULL;

/*
 * The table lives in a side struct hung off xtc_loop->user_data... but
 * we don't have that field.  Use a per-loop pointer hashed by loop
 * pointer in a tiny global table.  For M8 four-loop typical use this
 * is fine; M9 adds a proper field.
 */
#define LOOP_TABLE_MAX 64
struct lt_entry { xtc_loop_t *loop; struct xtc_proc_table *tbl; };
static pthread_mutex_t __lt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lt_entry __lt[LOOP_TABLE_MAX];

/* M11.5b: invalidate the registry entry for `loop` and free its
 * table.  Called from xtc_loop_fini.  Idempotent. */
void
__xtc_proc_loop_unregister(xtc_loop_t *loop)
{
	int i;
	struct xtc_proc_table *tbl = NULL;
	(void)pthread_mutex_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (__lt[i].loop == loop) {
			tbl = __lt[i].tbl;
			__lt[i].loop = NULL;
			__lt[i].tbl = NULL;
			break;
		}
	}
	(void)pthread_mutex_unlock(&__lt_lock);
	if (tbl != NULL) {
		/* Free still-live process headers.  By contract a loop
		 * being finalized has no live procs left, but we defend
		 * the path anyway. */
		if (tbl->slots != NULL) {
			size_t k;
			for (k = 0; k < tbl->cap; k++) {
				struct xtc_proc *p = tbl->slots[k].proc;
				if (p != NULL) {
					(void)pthread_mutex_destroy(&p->mbox_lock);
					__os_free(p);
					tbl->slots[k].proc = NULL;
				}
			}
			free(tbl->slots);
		}
		(void)pthread_mutex_destroy(&tbl->lock);
		free(tbl);
	}
}

static struct xtc_proc_table *
__table_for(xtc_loop_t *loop, int create)
{
	int i;
	struct xtc_proc_table *t = NULL;
	(void)pthread_mutex_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (__lt[i].loop == loop) { t = __lt[i].tbl; goto out; }
	}
	if (!create) goto out;
	if ((t = calloc(1, sizeof *t)) == NULL) goto out;
	(void)pthread_mutex_init(&t->lock, NULL);
	t->inited = 1;
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (__lt[i].loop == NULL) {
			__lt[i].loop = loop;
			__lt[i].tbl = t;
			goto out;
		}
	}
	free(t); t = NULL;
out:
	(void)pthread_mutex_unlock(&__lt_lock);
	return t;
}

static int
__table_alloc_slot(struct xtc_proc_table *t, struct xtc_proc *p,
                   uint16_t *out_local, uint32_t *out_gen)
{
	size_t i;
	int rc = XTC_OK;
	(void)pthread_mutex_lock(&t->lock);
	for (i = 0; i < t->cap; i++) {
		if (t->slots[i].proc == NULL) {
			t->slots[i].proc = p;
			*out_local = (uint16_t)i;
			*out_gen   = ++t->slots[i].gen;
			t->n_used++;
			goto out;
		}
	}
	/* Grow. */
	{
		size_t new_cap = t->cap == 0 ? 16 : t->cap * 2;
		struct xtc_proc_slot *ns = realloc(t->slots,
		    new_cap * sizeof *ns);
		if (ns == NULL) { rc = XTC_E_NOMEM; goto out; }
		for (i = t->cap; i < new_cap; i++) {
			ns[i].proc = NULL;
			ns[i].gen = 0;
		}
		t->slots = ns;
		t->cap = new_cap;
	}
	{
		size_t idx = t->n_used;     /* first new slot */
		t->slots[idx].proc = p;
		*out_local = (uint16_t)idx;
		*out_gen   = ++t->slots[idx].gen;
		t->n_used++;
	}
out:
	(void)pthread_mutex_unlock(&t->lock);
	return rc;
}

static struct xtc_proc *
__table_lookup(struct xtc_proc_table *t, uint16_t local_id, uint32_t gen)
{
	struct xtc_proc *p = NULL;
	(void)pthread_mutex_lock(&t->lock);
	if (local_id < t->cap &&
	    t->slots[local_id].proc != NULL &&
	    t->slots[local_id].gen == gen)
		p = t->slots[local_id].proc;
	(void)pthread_mutex_unlock(&t->lock);
	return p;
}

static void
__table_release(struct xtc_proc_table *t, uint16_t local_id)
{
	(void)pthread_mutex_lock(&t->lock);
	if (local_id < t->cap) {
		if (t->slots[local_id].proc != NULL) {
			t->slots[local_id].proc = NULL;
			t->n_used--;
		}
	}
	(void)pthread_mutex_unlock(&t->lock);
}

/* ---------- mailbox plumbing ---------- */

static void
__mbox_push_locked(struct xtc_proc *p, struct envelope *e)
{
	e->next = NULL;
	if (p->mbox_tail == NULL) p->mbox_head = p->mbox_tail = e;
	else { p->mbox_tail->next = e; p->mbox_tail = e; }
	p->mbox_n++;
}

static struct envelope *
__mbox_pop_locked(struct xtc_proc *p)
{
	struct envelope *e = p->mbox_head;
	if (e == NULL) return NULL;
	p->mbox_head = e->next;
	if (p->mbox_head == NULL) p->mbox_tail = NULL;
	p->mbox_n--;
	return e;
}

static int
__mbox_deliver(struct xtc_proc *p, struct envelope *e)
{
	int armed;
	(void)pthread_mutex_lock(&p->mbox_lock);
	/* Reject if proc is dead, or capped and full.
	 * Note: precedence bug fix -- the original used
	 *   if (!alive || cap > 0 ? mbox_n >= cap : 0)
	 * which parses as (!alive || cap>0) ? ... and produced
	 * surprising behaviour on platforms where alive timing
	 * differed.  Explicit parens. */
	if (!p->alive || (p->mbox_cap > 0 && p->mbox_n >= p->mbox_cap)) {
		(void)pthread_mutex_unlock(&p->mbox_lock);
		__env_free(e);
		return XTC_E_AGAIN;
	}
	/* Race window: the alive/capacity check passed but the envelope
	 * is not yet linked.  A test pauses a sender here, exits or
	 * fills the target, then releases to confirm the delivery still
	 * observes a consistent mailbox state under the lock. */
	XTC_INJECTION_POINT("proc.mbox.pre_push");
	__mbox_push_locked(p, e);
	armed = p->waker_armed;
	(void)pthread_mutex_unlock(&p->mbox_lock);
	if (armed) {
		/* Record the wake cause so xtc_proc_wait_fd / etc. can
		 * tell why we resumed. */
		if (p->task != NULL)
			p->task->wake_revents |= XTC_WAIT_MAILBOX;
		(void)xtc_waker_wake(&p->recv_waker);
	}
	return XTC_OK;
}

/* ---------- spawn entry trampoline ---------- */

/* Forward decls. */
static void __notify_links_and_monitors(struct xtc_proc *p);

static intptr_t
__proc_entry(void *arg)
{
	struct xtc_proc *p = arg;
	int reason;
	__current_proc = p;

	if ((reason = setjmp(p->exit_jb)) == 0) {
		p->exit_jb_set = 1;
		p->fn(p->arg);
		p->exit_reason = 0;        /* normal */
	} else {
		p->exit_reason = reason - 1;  /* offset so 0 is reachable */
	}

	p->alive = 0;
	/* __notify_links_and_monitors frees p (releases the slot,
	 * destroys the mailbox lock, frees the struct), so snapshot the
	 * exit reason before the call -- reading p->exit_reason after it
	 * is a use-after-free. */
	reason = p->exit_reason;
	__notify_links_and_monitors(p);

	__current_proc = NULL;
	return reason;
}

/*
 * PUBLIC: int xtc_proc_spawn __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *));
 */
int
xtc_proc_spawn(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
               const xtc_proc_opts_t *opts, xtc_pid_t *out_pid)
{
	struct xtc_proc *p;
	struct xtc_proc_table *tbl;
	xtc_task_t *t;
	uint16_t local;
	uint32_t gen;
	int rc;

	if (XTC_UNLIKELY(loop == NULL || fn == NULL)) return XTC_E_INVAL;

	if (XTC_UNLIKELY((tbl = __table_for(loop, 1)) == NULL)) return XTC_E_NOMEM;

	if (XTC_UNLIKELY((rc = __os_calloc(1, sizeof *p, (void **)&p)) != XTC_OK))
		return rc;
	(void)pthread_mutex_init(&p->mbox_lock, NULL);
	p->loop = loop;
	p->fn = fn;
	p->arg = arg;
	p->alive = 1;
	p->mbox_cap = (opts != NULL && opts->mailbox_cap > 0)
	    ? opts->mailbox_cap : 4096;

	if ((rc = __table_alloc_slot(tbl, p, &local, &gen)) != XTC_OK) {
		(void)pthread_mutex_destroy(&p->mbox_lock);
		__os_free(p);
		return rc;
	}
	p->pid.loop_id  = (uint16_t)(loop->exec_id < 0 ? 0 : loop->exec_id + 1);
	p->pid.local_id = local;
	p->pid.gen      = gen;

	/* Spawn the underlying coroutine. */
	if ((rc = xtc_async(loop, __proc_entry, p, &t)) != XTC_OK) {
		__table_release(tbl, local);
		(void)pthread_mutex_destroy(&p->mbox_lock);
		__os_free(p);
		return rc;
	}
	p->task = t;
	p->coro = (struct xtc_coro *)t->user;

	if (out_pid) *out_pid = p->pid;
	return XTC_OK;
}

/* PUBLIC: xtc_pid_t xtc_self __P((void)); */
xtc_pid_t
xtc_self(void)
{
	return __current_proc != NULL ? __current_proc->pid : XTC_PID_NONE;
}

/* ---------- send ---------- */

static struct xtc_proc *
__resolve(xtc_pid_t pid, xtc_loop_t **out_loop_for_send)
{
	xtc_loop_t *target_loop = NULL;
	struct xtc_proc_table *tbl;
	struct xtc_proc *p;

	/* Strategy 1: if we are running on a loop, prefer same-loop and
	 * exec-sibling lookups (cheapest). */
	if (__xtc_current_loop != NULL) {
		uint16_t my_id = (uint16_t)(__xtc_current_loop->exec_id < 0
		    ? 0 : __xtc_current_loop->exec_id + 1);
		if (pid.loop_id == my_id)
			target_loop = __xtc_current_loop;
		else if (__xtc_current_loop->exec != NULL) {
			struct xtc_exec *e = __xtc_current_loop->exec;
			int idx = (int)pid.loop_id - 1;
			if (idx >= 0 && idx < xtc_exec_n_loops(e))
				target_loop = xtc_exec_loop(e, idx);
		}
	}

	/*
	 * Strategy 2 (fallback for senders called from the main thread
	 * before xtc_loop_run): walk the global loop table to find the
	 * loop whose encoded ID matches this pid.  Linear in number of
	 * loops, which is small (<=64 in M8).
	 */
	if (target_loop == NULL) {
		int i;
		(void)pthread_mutex_lock(&__lt_lock);
		for (i = 0; i < LOOP_TABLE_MAX; i++) {
			xtc_loop_t *l = __lt[i].loop;
			uint16_t lid;
			if (l == NULL) continue;
			lid = (uint16_t)(l->exec_id < 0 ? 0 : l->exec_id + 1);
			if (lid == pid.loop_id) {
				target_loop = l;
				break;
			}
		}
		(void)pthread_mutex_unlock(&__lt_lock);
	}

	if (target_loop == NULL) return NULL;
	if (out_loop_for_send) *out_loop_for_send = target_loop;
	if ((tbl = __table_for(target_loop, 0)) == NULL) return NULL;
	p = __table_lookup(tbl, pid.local_id, pid.gen);
	return p;
}

/* PUBLIC: int xtc_send __P((xtc_pid_t, const void *, size_t)); */
int
xtc_send(xtc_pid_t to, const void *data, size_t size)
{
	struct xtc_proc *p;
	xtc_loop_t *target;
	struct envelope *e;

	if (XTC_UNLIKELY(size > 0 && data == NULL)) return XTC_E_INVAL;
	if (XTC_UNLIKELY(xtc_pid_is_none(to))) return XTC_E_INVAL;

	p = __resolve(to, &target);
	if (XTC_UNLIKELY(p == NULL || !p->alive)) return XTC_E_INVAL;

	/* Guard against size_t overflow in the envelope allocation:
	 * a size near SIZE_MAX would wrap sizeof *e + size to a small
	 * value, malloc would succeed, and the memcpy below would
	 * overflow the heap.  Reject before allocating. */
	if (XTC_UNLIKELY(size > SIZE_MAX - sizeof *e)) return XTC_E_INVAL;

	e = __env_alloc(size);
	if (XTC_UNLIKELY(e == NULL)) return XTC_E_NOMEM;
	e->next = NULL;
	e->from = xtc_self();
	e->size = size;
	if (size > 0) memcpy(e->data, data, size);

	return __mbox_deliver(p, e);
}

/* PUBLIC: int xtc_exit_pid __P((xtc_pid_t, int)); */
/*
 * Asynchronous, cross-process exit signal.  The target proc raises
 * an exit at its next yield/recv/wakeup point with the supplied
 * reason.  Idempotent: a second xtc_exit_pid before the first is
 * processed is a no-op (first one wins).
 *
 * This is the BEAM-style "kill" used by supervisors implementing
 * one_for_all and rest_for_one strategies, where the supervisor
 * needs to terminate sibling children that haven't crashed on
 * their own.
 */
int
xtc_exit_pid(xtc_pid_t target, int reason)
{
	struct xtc_proc *p;
	int expected = 0, desired;
	if (XTC_UNLIKELY(xtc_pid_is_none(target))) return XTC_E_INVAL;
	p = __resolve(target, NULL);
	if (XTC_UNLIKELY(p == NULL || !p->alive)) return XTC_E_INVAL;

	/* Encode reason so 0 means "no kill pending".  Negative reasons
	 * are clamped to -1 so the encoded value stays nonzero. */
	desired = (reason == 0) ? 1 : reason + 1;
	(void)atomic_compare_exchange_strong_explicit(&p->kill_pending,
	    &expected, desired, memory_order_release, memory_order_relaxed);

	/* Best-effort: if the target is parked on its recv waker, wake
	 * it so it can observe the kill.  If it's runnable already this
	 * is a no-op. */
	(void)pthread_mutex_lock(&p->mbox_lock);
	if (p->waker_armed) {
		xtc_waker_wake(&p->recv_waker);
		p->waker_armed = 0;
	}
	(void)pthread_mutex_unlock(&p->mbox_lock);
	return XTC_OK;
}

/* ---------- receive ---------- */

static int
__match_first(const void *data, size_t size, void *u)
{
	(void)data; (void)size; (void)u;
	return 1;
}

static int
__do_recv(xtc_match_fn match, void *u, void **out, size_t *out_size,
          int64_t timeout_ns)
{
	struct xtc_proc *self = __current_proc;
	struct envelope *e, **link;
	int64_t deadline = -1;

	if (self == NULL) return XTC_E_INVAL;
	if (out == NULL || out_size == NULL) return XTC_E_INVAL;

	/* Defensive zero: the contract is that callers may pass
	 * uninitialised storage and check for non-NULL after a
	 * successful return.  Without this, a recv that returns
	 * XTC_E_AGAIN (timeout) would leave *out as garbage stack
	 * memory, and a caller doing `if (m) free(m)` would crash. */
	*out = NULL;
	*out_size = 0;

	/* Asynchronous kill check.  If another proc has signalled us via
	 * xtc_exit_pid, raise the exit now (longjmp) instead of receiving
	 * any more messages. */
	{
		int kp = atomic_load_explicit(&self->kill_pending,
		    memory_order_acquire);
		if (kp != 0)
			xtc_exit_self(kp - 1);
	}

	if (timeout_ns >= 0) {
		int64_t now;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}

	/* BEAM recv-mark: if the predicate has changed since the last
	 * call, invalidate the mark and re-test the whole save queue.
	 * If unchanged, we'll skip past `recv_mark` on the walk below. */
	if (self->last_match_fn != match || self->last_match_user != u) {
		self->last_match_fn = match;
		self->last_match_user = u;
		self->recv_mark = NULL;
	}

	for (;;) {
		struct envelope *skip_until = self->recv_mark;
		int past_mark = (skip_until == NULL);
		/* Walk save queue first.  Skip entries up to and including
		 * recv_mark (already tested with this predicate). */
		link = &self->save_head;
		while ((e = *link) != NULL) {
			if (!past_mark) {
				if (e == skip_until) past_mark = 1;
				link = &e->next;
				continue;
			}
			if (match(e->data, e->size, u)) {
				/* Unlink. */
				*link = e->next;
				if (self->save_tail == e) {
					/* Walk to recompute tail.  O(N) but
					 * portable; the back-pointer trick using
					 * offsetof(envelope, next) was suspected of
					 * miscompilation on MinGW Windows. */
					struct envelope *t = self->save_head;
					if (t == NULL) self->save_tail = NULL;
					else {
						while (t->next != NULL) t = t->next;
						self->save_tail = t;
					}
				}
				goto deliver;
			}
			link = &e->next;
		}

		/* Pull from mailbox; for each, match or move to save. */
		(void)pthread_mutex_lock(&self->mbox_lock);
		while ((e = __mbox_pop_locked(self)) != NULL) {
			(void)pthread_mutex_unlock(&self->mbox_lock);
			if (match(e->data, e->size, u))
				goto deliver;
			/* Append to save queue. */
			e->next = NULL;
			if (self->save_tail == NULL) self->save_head = self->save_tail = e;
			else { self->save_tail->next = e; self->save_tail = e; }
			(void)pthread_mutex_lock(&self->mbox_lock);
		}
		(void)pthread_mutex_unlock(&self->mbox_lock);

		/* Update recv_mark: everything in save_queue has now been
		 * tested against this predicate.  Next call with the same
		 * predicate will skip past this point in the queue. */
		self->recv_mark = self->save_tail;

		/* Nothing to deliver.  Check timeout. */
		if (timeout_ns == 0) return XTC_E_AGAIN;

		/* Park: register waker, yield, retry. */
		(void)xtc_task_waker(self->task, &self->recv_waker);
		(void)pthread_mutex_lock(&self->mbox_lock);
		self->waker_armed = 1;
		(void)pthread_mutex_unlock(&self->mbox_lock);

		if (deadline >= 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			if (now >= deadline) return XTC_E_AGAIN;
			(void)xtc_task_park_on_timer(self->task, deadline - now);
		}
		xtc_yield();
		/*
		 * On resume we may have run inside another proc's fiber
		 * (which clobbered __current_proc).  Restore our pointer
		 * so post-yield code continues to see itself.
		 */
		__current_proc = self;

		/* Re-check kill flag after yielding back. */
		{
			int kp = atomic_load_explicit(&self->kill_pending,
			    memory_order_acquire);
			if (kp != 0)
				xtc_exit_self(kp - 1);
		}

		(void)pthread_mutex_lock(&self->mbox_lock);
		self->waker_armed = 0;
		(void)pthread_mutex_unlock(&self->mbox_lock);
	}

deliver:
	/* Cancel any pending recv timer.  Safe no-op if it never armed
	 * or has already fired.  Without this, a recv that was woken by
	 * the waker leaves a stale timer in the heap, keeping the loop
	 * alive until the deadline (a needless wait of up to timeout_ns). */
	if (self->task->park_timer != NULL) {
		(void)xtc_timer_cancel(self->task->park_timer);
		self->task->park_timer = NULL;
	}
	{
		void *buf = NULL;
		if (e->size > 0) {
			buf = malloc(e->size);
			if (buf == NULL) {
				/* Put it back at the head of save queue
				 * to preserve ordering. */
				e->next = self->save_head;
				self->save_head = e;
				if (self->save_tail == NULL) self->save_tail = e;
				return XTC_E_NOMEM;
			}
			memcpy(buf, e->data, e->size);
		}
		*out = buf;
		*out_size = e->size;
		__env_free(e);
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_recv __P((void **, size_t *, int64_t)); */
int
xtc_recv(void **out, size_t *out_size, int64_t timeout_ns)
{
	/* Wrapper around __do_recv with the always-match predicate.
	 * Annotated XTC_MUSTTAIL so the compiler emits a jmp, not a
	 * call+ret -- keeps the recv fast path single-frame. */
	return XTC_MUSTTAIL __do_recv(__match_first, NULL, out, out_size,
	    timeout_ns);
}

/* PUBLIC: int xtc_recv_match __P((xtc_match_fn, void *, void **, size_t *, int64_t)); */
int
xtc_recv_match(xtc_match_fn fn, void *u, void **out, size_t *out_size,
               int64_t timeout_ns)
{
	if (XTC_UNLIKELY(fn == NULL)) return XTC_E_INVAL;
	/* XTC_MUSTTAIL: delegate to __do_recv as a tail call. */
	return XTC_MUSTTAIL __do_recv(fn, u, out, out_size, timeout_ns);
}

/* Predicate context for xtc_recv_correlate. */
struct corr_ctx {
	const unsigned char *corr;
	size_t               corr_size;
};

static int
__corr_match(const void *data, size_t size, void *u)
{
	struct corr_ctx *c = u;
	if (c == NULL || size < c->corr_size) return 0;
	return memcmp(data, c->corr, c->corr_size) == 0;
}

/* PUBLIC: int xtc_recv_correlate __P((const void *, size_t, int, xtc_msg_t *, int *, int64_t)); */
int
xtc_recv_correlate(const void *corr_value, size_t corr_size,
                   int n_expected, xtc_msg_t *out_msgs,
                   int *out_n, int64_t timeout_ns)
{
	struct corr_ctx ctx;
	int64_t deadline = -1;
	int collected = 0;
	int rc;

	if (corr_value == NULL || corr_size == 0 ||
	    n_expected <= 0 || out_msgs == NULL || out_n == NULL)
		return XTC_E_INVAL;

	ctx.corr = (const unsigned char *)corr_value;
	ctx.corr_size = corr_size;
	*out_n = 0;

	if (timeout_ns >= 0) {
		int64_t now;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}

	while (collected < n_expected) {
		int64_t per_call_to = -1;
		if (deadline >= 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			if (now >= deadline) break;
			per_call_to = deadline - now;
		}
		rc = __do_recv(__corr_match, &ctx,
		    &out_msgs[collected].data,
		    &out_msgs[collected].size,
		    per_call_to);
		if (rc == XTC_OK) {
			collected++;
		} else {
			/* AGAIN (timeout) or other error: stop. */
			break;
		}
	}

	*out_n = collected;
	return (collected == n_expected) ? XTC_OK : XTC_E_AGAIN;
}

/* PUBLIC: int xtc_proc_wait_fd __P((int, uint32_t, int64_t, uint32_t *)); */
int
xtc_proc_wait_fd(int fd, uint32_t interest, int64_t timeout_ns,
                 uint32_t *out_revents)
{
	struct xtc_proc *self = __current_proc;
	uint32_t revents;
	int had_timer = 0;
	int had_fd = 0;

	if (out_revents == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (self == NULL) return XTC_E_INVAL;

	*out_revents = 0;

	/* Check kill-pending up front (same convention as xtc_recv). */
	{
		int kp = atomic_load_explicit(&self->kill_pending,
		    memory_order_acquire);
		if (kp != 0) xtc_exit_self(kp - 1);
	}

	/* Fast path: if a message is already queued or the fd is already
	 * ready, just return without yielding.  We can answer the mailbox
	 * question without an actual recv call by peeking the queue. */
	(void)pthread_mutex_lock(&self->mbox_lock);
	if (self->mbox_n > 0 || self->save_head != NULL) {
		*out_revents |= XTC_WAIT_MAILBOX;
	}
	(void)pthread_mutex_unlock(&self->mbox_lock);
	if (*out_revents & XTC_WAIT_MAILBOX) return XTC_OK;

	/* Slow path: arm the recv waker, register the fd, optionally
	 * arm a timeout timer, then yield.  We bypass
	 * xtc_task_park_on_fd / _on_timer because those wrappers reject
	 * having both set; for wait_fd we need fd + timer + waker
	 * simultaneously. */
	self->task->wake_revents = 0;

	if (xtc_io_reg_fd(self->task->loop->io, fd, interest,
	    self->task) != XTC_OK)
		return XTC_E_INTERNAL;
	self->task->park_fd = fd;
	had_fd = 1;

	if (timeout_ns >= 0) {
		/* Inline timer registration matching xtc_task_park_on_timer
		 * but without the mutual-exclusion check. */
		xtc_timer_t *t = NULL;
		int64_t now_ns = 0;
		int trc = __os_calloc(1, sizeof(*t), (void **)&t);
		if (trc != XTC_OK || t == NULL ||
		    __os_clock_mono(&now_ns) != XTC_OK) {
			if (t) __os_free(t);
			(void)xtc_io_del_fd(self->task->loop->io, fd);
			self->task->park_fd = -1;
			return XTC_E_INTERNAL;
		}
		t->deadline_ns = now_ns + timeout_ns;
		t->cb = NULL;
		t->user = NULL;
		t->waiter = self->task;
		t->heap_idx = -1;
		t->cancelled = 0;
		t->fired = 0;
		t->loop = self->task->loop;
		if (__xtc_timer_heap_push(self->task->loop, t) != XTC_OK) {
			__os_free(t);
			(void)xtc_io_del_fd(self->task->loop->io, fd);
			self->task->park_fd = -1;
			return XTC_E_INTERNAL;
		}
		t->all_next = self->task->loop->all_timers;
		self->task->loop->all_timers = t;
		self->task->park_timer = t;
		had_timer = 1;
	}

	(void)xtc_task_waker(self->task, &self->recv_waker);
	(void)pthread_mutex_lock(&self->mbox_lock);
	self->waker_armed = 1;
	(void)pthread_mutex_unlock(&self->mbox_lock);

	xtc_yield();
	/* Restore __current_proc -- another fiber may have clobbered it. */
	__current_proc = self;

	(void)pthread_mutex_lock(&self->mbox_lock);
	self->waker_armed = 0;
	(void)pthread_mutex_unlock(&self->mbox_lock);

	/* Re-check kill-pending after yielding back. */
	{
		int kp = atomic_load_explicit(&self->kill_pending,
		    memory_order_acquire);
		if (kp != 0) xtc_exit_self(kp - 1);
	}

	/* Sample wake_revents.  The dispatcher / mbox_deliver / timer cb
	 * have set the bits we care about. */
	revents = self->task->wake_revents;
	self->task->wake_revents = 0;

	/* Cleanup: unregister fd if still parked, cancel timer. */
	(void)had_fd;   /* unused but documents intent */
	if (self->task->park_fd >= 0) {
		(void)xtc_io_del_fd(self->task->loop->io, self->task->park_fd);
		self->task->park_fd = -1;
	}
	if (had_timer && self->task->park_timer != NULL) {
		(void)xtc_timer_cancel(self->task->park_timer);
		self->task->park_timer = NULL;
	}

	/* Check the mailbox again -- a message may have arrived without
	 * tripping the waker race-window. */
	(void)pthread_mutex_lock(&self->mbox_lock);
	if (self->mbox_n > 0 || self->save_head != NULL) {
		revents |= XTC_WAIT_MAILBOX;
	}
	(void)pthread_mutex_unlock(&self->mbox_lock);

	*out_revents = revents;

	/* Decide return code: if only timeout fired, return XTC_E_AGAIN. */
	if ((revents & ~(uint32_t)XTC_WAIT_TIMEOUT) == 0 && timeout_ns >= 0)
		return XTC_E_AGAIN;
	return XTC_OK;
}

/* ---------- exit / link / monitor ---------- */

/* PUBLIC: int xtc_exit_self __P((int)); */
int
xtc_exit_self(int reason)
{
	struct xtc_proc *self = __current_proc;
	if (self == NULL || !self->exit_jb_set) return XTC_E_INVAL;
	longjmp(self->exit_jb, reason + 1);
	/* NOTREACHED */
	return XTC_OK;
}

/* PUBLIC: int xtc_link __P((xtc_pid_t)); */
int
xtc_link(xtc_pid_t other)
{
	struct xtc_proc *self = __current_proc;
	struct xtc_proc *peer;
	struct link_entry *le;
	if (self == NULL) return XTC_E_INVAL;
	peer = __resolve(other, NULL);
	if (peer == NULL || !peer->alive) return XTC_E_INVAL;
	le = __link_alloc();
	if (le == NULL) return XTC_E_NOMEM;
	le->peer = other;
	le->next = self->links;
	self->links = le;
	/* Symmetric: add ourselves to peer's links too. */
	{
		struct link_entry *pe = __link_alloc();
		if (pe != NULL) {
			pe->peer = self->pid;
			pe->next = peer->links;
			peer->links = pe;
		}
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_unlink __P((xtc_pid_t)); */
int
xtc_unlink(xtc_pid_t other)
{
	struct xtc_proc *self = __current_proc;
	struct link_entry **pp;
	if (self == NULL) return XTC_E_INVAL;
	for (pp = &self->links; *pp != NULL; pp = &(*pp)->next) {
		if (xtc_pid_eq((*pp)->peer, other)) {
			struct link_entry *e = *pp;
			*pp = e->next;
			__link_free(e);
			break;
		}
	}
	return XTC_OK;
}

static _Atomic uint64_t __mon_ref_seq = 0;

/* PUBLIC: int xtc_monitor __P((xtc_pid_t, uint64_t *)); */
int
xtc_monitor(xtc_pid_t target, uint64_t *out_ref)
{
	struct xtc_proc *self = __current_proc;
	struct xtc_proc *peer;
	struct mon_entry *me;
	if (self == NULL) return XTC_E_INVAL;
	peer = __resolve(target, NULL);
	if (peer == NULL || !peer->alive) return XTC_E_INVAL;
	me = __mon_alloc();
	if (me == NULL) return XTC_E_NOMEM;
	me->ref = atomic_fetch_add_explicit(&__mon_ref_seq, 1,
	    memory_order_relaxed) + 1;
	me->target = target;
	me->watcher = self->pid;
	me->next = self->monitors;
	self->monitors = me;
	{
		struct mon_entry *m2 = __mon_alloc();
		if (m2 != NULL) {
			*m2 = *me;
			m2->next = peer->monitored_by;
			peer->monitored_by = m2;
		}
	}
	if (out_ref) *out_ref = me->ref;
	return XTC_OK;
}

/* On exit: notify links + monitors. */
static void
__notify_links_and_monitors(struct xtc_proc *p)
{
	struct link_entry *le, *next_le;
	struct mon_entry  *me, *next_me;
	XTC_PACK_PUSH
	struct {
		uint8_t kind;
		int     reason;
		xtc_pid_t pid;
	} XTC_PACKED exit_signal = {
		.kind = 'E', .reason = p->exit_reason, .pid = p->pid
	};
	struct {
		uint8_t kind;
		uint64_t ref;
		xtc_pid_t pid;
		int     reason;
	} XTC_PACKED down_signal;
	XTC_PACK_POP

	for (le = p->links; le != NULL; le = next_le) {
		next_le = le->next;
		(void)xtc_send(le->peer, &exit_signal, sizeof exit_signal);
		__link_free(le);
	}
	p->links = NULL;

	for (me = p->monitored_by; me != NULL; me = next_me) {
		next_me = me->next;
		down_signal.kind = 'D';
		down_signal.ref  = me->ref;
		down_signal.pid  = p->pid;
		down_signal.reason = p->exit_reason;
		(void)xtc_send(me->watcher, &down_signal, sizeof down_signal);
		__mon_free(me);
	}
	p->monitored_by = NULL;

	for (me = p->monitors; me != NULL; me = next_me) {
		next_me = me->next;
		__mon_free(me);
	}
	p->monitors = NULL;

	/* Drain mailbox + save queue. */
	{
		struct envelope *e, *n;
		(void)pthread_mutex_lock(&p->mbox_lock);
		for (e = p->mbox_head; e != NULL; e = n) { n = e->next; __env_free(e); }
		p->mbox_head = p->mbox_tail = NULL;
		(void)pthread_mutex_unlock(&p->mbox_lock);
		for (e = p->save_head; e != NULL; e = n) { n = e->next; __env_free(e); }
		p->save_head = p->save_tail = NULL;
	}

	/* Release slot. */
	{
		struct xtc_proc_table *tbl = __table_for(p->loop, 0);
		if (tbl != NULL) __table_release(tbl, p->pid.local_id);
	}
	(void)pthread_mutex_destroy(&p->mbox_lock);
	__os_free(p);
}
