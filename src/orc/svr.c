/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/svr.c
 *	gen_server implementation.  The server is an xtc_proc; it
 *	dispatches each incoming envelope to handle_call / handle_cast /
 *	handle_info based on a one-byte tag in the envelope header.
 *
 *	Wire format (request side):
 *	  byte 0:   'C' (call) | 'X' (cast) | 'I' (info-direct, unused)
 *	  bytes 1..: payload-specific
 *
 *	For a call (byte 1 selects the reply route):
 *	  'C' 's' id[8] payload  -- off-proc caller: id names an in-flight
 *	                            reply slot (see the slot table below)
 *	  'C' 'p' pid[8] tag[8] payload -- in-proc caller: reply by mailbox
 *
 *	In-proc reply (to the caller's mailbox):
 *	  payload tag[8] SVR_REPLY_MAGIC[8]
 *	The magic keeps an ordinary application message from being taken
 *	as a reply; the 64-bit tag is never reused.  The header TRAILS the
 *	payload so the caller is handed the received block itself (frame
 *	moved with xtc_send_owned, header dropped by just the size): a
 *	reply crosses with one copy, the server's into the frame.
 *
 *	For a cast:
 *	  byte 0:    'X'
 *	  bytes 1..: user payload
 *
 *	Anything else (kind byte not 'C'/'X') is delivered to
 *	handle_info verbatim.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "sim_int.h"       /* XTC_SIM_BUGGIFY; __xtc_sim_active */
#include "xtc_svr.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "xtc_async.h"    /* xtc_yield: buggify delay-dispatch site */
#include "xtc_inject.h"   /* svr.reply.oom fault-injection point */
#include "xtc_dst_inject.h"
#include "xtc_app.h"  /* XTC_APP_SHUTDOWN_MSG: a child server's drain */
#include "loop_int.h" /* __xtc_loop_may_wait: wait_init */
#include "proc_int.h" /* __xtc_proc_start_ack, __xtc_proc_self_loop */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int __recv_reply_for_tag(uint64_t tag, uint64_t mref, xtc_pid_t target,
    void **out, size_t *out_size, int64_t timeout_ns, xtc_abort_token_t *tok);

#define SVR_REPLY_HDR 16
static const uint8_t SVR_REPLY_MAGIC[8] = {
	0xfe, 'x', 't', 'c', 's', 'v', 'r', 0xa5
};

/* A pid-routed call frame: 'C' 'p' <xtc_pid_t> <tag:8> <payload>. */
#define SVR_PCALL_HDR (2 + sizeof(xtc_pid_t) + 8)

struct xtc_svr {
	xtc_loop_t           *loop;
	xtc_svr_callbacks_t   cb;
	void                 *state;
	/* Stored by xtc_svr_start and again (same value) by the server
	 * proc itself, read from any thread. */
	xtc_atomic_pid_t      pid;
	xtc_notify_t         *stopped;
	int stop_requested;
	int alive;
	/* handle_continue: a callback may arm a continuation via
	 * xtc_svr_continue(); the server runs handle_continue(state, cont)
	 * before its next recv, so it can finish expensive init/post-work
	 * off the caller's critical path, race-free. */
	int                   cont_pending;
	void                 *cont_arg;
	void                 *inflight;   /* message being dispatched */
	/* wait_init (2.2): the server signals `started` after init, with
	 * init's code in init_rc.  NULL when nobody waits. */
	xtc_notify_t *started;
	int init_rc;
	int start_told; /* started has been signalled */
	/* xtc_svr_child_main: the server is a supervisor's child; its own
	 * at-exit hook frees it (there is no handle to join). */
	int child;
	/* Two parties release the handle: the server's exit hook, and its
	 * owner (xtc_svr_join, or __svr_orphan for a handle nobody will
	 * join: a failed wait_init start, or one whose caller was killed).
	 * Each adds one as its last access; the second frees. */
	int release;
};

/* The server whose callback is currently running on this thread, so
 * xtc_svr_continue() can find it.  A server proc runs its callbacks
 * only on its own loop thread, so a plain TLS pointer is correct.  That
 * holds only while servers are PINNED: clang keeps &__svr_cur live across
 * xtc_yield in __svr_entry, so a migratable server would need an
 * out-of-line accessor (see __current_proc in src/ptc/proc.c). */
static XTC_THREAD_LOCAL struct xtc_svr *__svr_cur;

/*
 * Reply slot of an off-proc (plain thread) caller.  It lives on the
 * caller's stack and is reachable ONLY through the slot table, by the id
 * the request carries -- never by a pointer in the message.  Every
 * access (reply store, signal, caller unlink) happens under the lock of
 * the id's bucket, and the caller unlinks before it returns, so a reply
 * that arrives after a timeout finds no slot and is discarded.  (Before
 * 1.55.1 the message carried &slot and a late reply wrote into the dead
 * frame: ASan stack-use-after-return / SEGV in xtc_notify_signal.)
 * The table is hashed by id so concurrent callers do not share a lock:
 * one global lock, held across the reply's signal, made every off-proc
 * call queue behind every other (+60-90% per call at 8-16 threads).
 */
struct __svr_reply_slot {
	struct __svr_reply_slot *next;
	uint64_t        id;
	xtc_notify_t   *done;
	void           *data;
	size_t          size;
	int             replied;
	xtc_pid_t target; /* the server called */
	int gone;         /* the server exited without replying */
};

#define SVR_SLOT_BUCKETS 64          /* power of two */
struct __svr_slot_bucket {
	_Alignas(XTC_CACHE_LINE) pthread_mutex_t lock;
	struct __svr_reply_slot *head;
};
static struct __svr_slot_bucket __svr_slot_tab[SVR_SLOT_BUCKETS];
static __os_once_t              __svr_slot_once = XTC_OS_ONCE_INIT;
static uint64_t __svr_slot_seq;

static void
__svr_slot_tab_init(void)
{
	int i;
	for (i = 0; i < SVR_SLOT_BUCKETS; i++)
		(void)pthread_mutex_init(&__svr_slot_tab[i].lock, NULL);
}

static struct __svr_slot_bucket *
__svr_slot_bucket(uint64_t id)
{
	(void)__os_call_once(&__svr_slot_once, __svr_slot_tab_init);
	return &__svr_slot_tab[id & (SVR_SLOT_BUCKETS - 1)];
}

struct xtc_svr_call {
	struct xtc_svr           *svr;
	uint64_t                  slot_id;     /* slot path (0 = pid path) */
	xtc_pid_t                 reply_pid;   /* pid-based path  */
	uint64_t                  reply_tag;
	int                       heap;        /* 1 == heap copy from _save */
};

/*
 * Slice length for reply waits.  A server's exit ends a wait at once (a
 * DOWN for an in-proc caller, the server's exit hook for a plain thread),
 * so the slice is only a backstop: the abort token is polled every
 * SVR_ABORT_SLICE_NS, and liveness every SVR_LIVE_SLICE_NS for the one
 * death that sends neither (the server's loop finalized under the call).
 * Through 2.2 every wait polled liveness every 25 ms.
 */
#define SVR_ABORT_SLICE_NS (25LL * 1000 * 1000)
#define SVR_LIVE_SLICE_NS (1000LL * 1000 * 1000)

/* Has the server exited?  Resolves the pid the same way xtc_send does,
 * from a proc or a plain thread. */
static int
__svr_gone(xtc_pid_t target)
{
	xtc_mailbox_stats_t st = XTC_MAILBOX_STATS_INIT;
	return xtc_proc_mailbox_stats(target, &st) != XTC_OK;
}

static void
__svr_slot_unlink_locked(struct __svr_slot_bucket *b,
    struct __svr_reply_slot *slot)
{
	struct __svr_reply_slot **pp;
	for (pp = &b->head; *pp != NULL; pp = &(*pp)->next)
		if (*pp == slot) {
			*pp = slot->next;
			return;
		}
}

/* ----- entry ----------------------------------------------------- */

/* Run any pending continuation(s): a callback armed one via
 * xtc_svr_continue(), and handle_continue may itself arm another.
 * Runs on the server's own loop thread, before the next recv. */
static void
__svr_run_continue(struct xtc_svr *s)
{
	while (s->cont_pending && s->cb.handle_continue != NULL &&
	    !xtc_atomic_load_i32(&s->stop_requested, xtc_mo_acquire)) {
		void *cont = s->cont_arg;
		s->cont_pending = 0;
		s->cont_arg = NULL;
		__svr_cur = s;
		(void)s->cb.handle_continue(s->state, cont);
		__svr_cur = NULL;
	}
	/* If no handle_continue is set, drop a stray arm. */
	s->cont_pending = 0;
	s->cont_arg = NULL;
}

/*
 * Proc at-exit hook: runs on EVERY exit of the server proc -- a normal
 * return, a handler that calls xtc_exit_self, a kill, a contained
 * fault.  Before 1.55.1 `stopped` was signalled only on the graceful
 * path, so after any other exit xtc_svr_join never returned XTC_OK and
 * the server object leaked; the message being dispatched leaked too.
 */
/*
 * Wake every plain-thread caller still waiting on server `pid`: the
 * server is exiting and will not reply.  A slot is linked before its
 * request is sent and a send to a server already exiting fails, so a
 * waiting slot is either here or its send failed.
 */
static void
__svr_slots_gone(xtc_pid_t pid)
{
	struct __svr_reply_slot *slot;
	int i;
	for (i = 0; i < SVR_SLOT_BUCKETS; i++) {
		struct __svr_slot_bucket *b = __svr_slot_bucket((uint64_t)i);
		(void)__xtc_mtx_lock(&b->lock);
		for (slot = b->head; slot != NULL; slot = slot->next)
			if (!slot->replied && xtc_pid_eq(slot->target, pid)) {
				slot->gone = 1;
				(void)xtc_notify_signal(slot->done);
			}
		(void)__xtc_mtx_unlock(&b->lock);
	}
}

/* Free a server handle and its notifies. */
static void
__svr_free(struct xtc_svr *s)
{
	if (s->stopped != NULL)
		xtc_notify_destroy(s->stopped);
	if (s->started != NULL)
		xtc_notify_destroy(s->started);
	__os_free(s);
}

/* One party (exit hook or owner) is done with s; the second frees it. */
static void
__svr_release(struct xtc_svr *s)
{
	if (xtc_atomic_fetch_add_i32(&s->release, 1, xtc_mo_acq_rel) == 1)
		__svr_free(s);
}

static void
__svr_exited(void *arg)
{
	struct xtc_svr *s = arg;
	if (__svr_cur == s)
		__svr_cur = NULL;
	if (s->inflight != NULL) {
		__os_free(s->inflight);
		s->inflight = NULL;
	}
	xtc_atomic_store_i32(&s->alive, 0, xtc_mo_release);
	__svr_slots_gone(xtc_atomic_pid_load(&s->pid));
	/* Killed before init returned: a waiting xtc_svr_start fails. */
	if (s->started != NULL && !s->start_told) {
		s->start_told = 1;
		s->init_rc = XTC_E_NOTFOUND;
		(void)xtc_notify_signal(s->started);
	}
	if (s->child) {
		__svr_free(s); /* a child server has no handle */
		return;
	}
	(void)xtc_notify_signal(s->stopped);
	__svr_release(s);
}

/* A handle nobody will join: stop the server; the handle goes when the
 * server has exited. */
static void
__svr_orphan(void *arg)
{
	struct xtc_svr *s = arg;
	(void)xtc_svr_stop(s);
	__svr_release(s);
}

/*
 * A synchronous start's wait (2.2: svr and fsm wait_init, a supervisor's
 * wait_children): wait for `started`, from a proc (parked) or a plain
 * thread.  On a proc a kill can land in the wait, and the caller then
 * never sees the handle: a scope finalizer hands obj to orphan(obj),
 * which stops it and frees it once it has exited.  Returns XTC_OK when
 * started was signalled, or XTC_E_RESOURCE (no scope).  The caller has
 * checked __xtc_loop_may_wait.
 */
struct __start_guard {
	void (*orphan)(void *);
	void *obj;
	int armed;
};

static void
__start_guard_fire(void *arg)
{
	struct __start_guard *g = arg;
	if (g->armed)
		g->orphan(g->obj);
	__os_free(g);
}

/* PUBLIC: int __xtc_orc_start_wait __P((xtc_notify_t *, void (*)(void *), void *)); */
int
__xtc_orc_start_wait(xtc_notify_t *started, void (*orphan)(void *), void *obj)
{
	struct __start_guard *g = NULL;
	xtc_scope_t *sc = NULL;
	int rc;

	if (!xtc_pid_is_none(xtc_self())) {
		if ((rc = __os_malloc(sizeof *g, (void **)&g)) != XTC_OK)
			return rc;
		g->orphan = orphan;
		g->obj = obj;
		g->armed = 1;
		if ((sc = xtc_scope_enter()) == NULL ||
		    xtc_scope_defer(sc, __start_guard_fire, g) != XTC_OK) {
			g->armed = 0;
			if (sc != NULL)
				xtc_scope_leave(sc);
			__os_free(g);
			return XTC_E_RESOURCE;
		}
	}
	rc = xtc_notify_wait(started, -1);
	if (g != NULL) {
		g->armed = 0;
		xtc_scope_leave(sc); /* frees g */
	}
	return rc;
}

static void
__svr_entry(void *arg)
{
	struct xtc_svr *s = arg;
	int hooked;
	int init_rc = XTC_OK;
	xtc_atomic_pid_store(&s->pid, xtc_self());
	hooked = (xtc_proc_at_exit(__svr_exited, s) == XTC_OK);

	if (s->cb.init != NULL) {
		__svr_cur = s;
		init_rc = s->cb.init(s->state);
		__svr_cur = NULL;
	}
	/* A waiting xtc_svr_start returns now (on failure it orphans s, so
	 * s stays valid until our exit hook); a waiting supervisor is told
	 * the child is up.  Both before a continuation: that is the point of
	 * xtc_svr_continue. */
	if (s->started != NULL) {
		s->start_told = 1;
		s->init_rc = init_rc;
		(void)xtc_notify_signal(s->started);
	}
	if (init_rc != XTC_OK)
		goto out;
	__xtc_proc_start_ack();
	__svr_run_continue(s);   /* init may have armed a continuation */

	while (!xtc_atomic_load_i32(&s->stop_requested, xtc_mo_acquire)) {
		void  *msg = NULL;
		size_t size = 0;
		int    rc;
		int    cont = XTC_SVR_CONTINUE;
		uint8_t kind;

		rc = xtc_recv(&msg, &size, 100LL * 1000 * 1000);
		if (rc == XTC_E_AGAIN) continue;
		if (rc != XTC_OK) break;
		if (size == 0) { __os_free(msg); continue; }

		/* Buggify: under DST, occasionally YIELD after receiving a
		 * message but before dispatching it -- the message is already
		 * in hand (not re-queued, so it cannot be lost), so this is a
		 * strictly legal pessimal delay that lets other procs interleave
		 * between the server's recv and its dispatch, stressing the
		 * recv/dispatch window deterministically.  Gated on the site
		 * coin; a fresh per-call fault draw so it fires on a fraction
		 * of receives. */
		if (XTC_SIM_BUGGIFY("svr.recv.delay_dispatch") &&
		    xtc_sim_fault(200))
			xtc_yield();

		kind = ((uint8_t *)msg)[0];
		s->inflight = msg;

		/* Internal stop-kick: a single 'S' byte sent by xtc_svr_stop
		 * to wake us from the recv-poll.  Don't dispatch. */
		if (size == 1 && kind == 'S') {
			s->inflight = NULL;
			__os_free(msg);
			continue;
		}

		if (kind == 'C') {
			/* Call: byte 1 is the routing tag.
			 *   's' = slot routing  (bytes 2..9 = slot id,     payload at +10)
			 *   'p' = pid  routing  (bytes 2.. = xtc_pid_t, then the
			 *                       8-byte tag, payload at SVR_PCALL_HDR)
			 */
			struct xtc_svr_call call = {0};
			size_t hdr;
			call.svr = s;
			__svr_cur = s;   /* for xtc_svr_continue() during the callback */
			if (size >= 10 && ((uint8_t *)msg)[1] == 's') {
				memcpy(&call.slot_id, (uint8_t *)msg + 2, 8);
				hdr = 10;
			} else if (size >= SVR_PCALL_HDR &&
			    ((uint8_t *)msg)[1] == 'p') {
				memcpy(&call.reply_pid, (uint8_t *)msg + 2,
				    sizeof call.reply_pid);
				memcpy(&call.reply_tag, (uint8_t *)msg + 2 +
				    sizeof call.reply_pid, 8);
				hdr = SVR_PCALL_HDR;
			} else {
				/* Malformed call.  Skip. */
				s->inflight = NULL;
				__os_free(msg);
				continue;
			}
			if (s->cb.handle_call != NULL) {
				cont = s->cb.handle_call(s->state,
				    (uint8_t *)msg + hdr, size - hdr, &call);
			} else {
				/* No handler: send empty reply. */
				(void)xtc_svr_reply(&call, NULL, 0);
			}
		} else if (kind == 'X') {
			__svr_cur = s;
			if (s->cb.handle_cast != NULL) {
				cont = s->cb.handle_cast(s->state,
				    (uint8_t *)msg + 1, size - 1);
			} else if (s->cb.handle_info != NULL) {
				cont = s->cb.handle_info(s->state, msg, size);
			}
		} else {
			__svr_cur = s;
			if (s->cb.handle_info != NULL)
				cont = s->cb.handle_info(s->state, msg, size);
			else if (s->child &&
			    size == sizeof XTC_APP_SHUTDOWN_MSG &&
			    memcmp(msg, XTC_APP_SHUTDOWN_MSG, size) == 0)
				cont = XTC_SVR_STOP; /* a drain, unhandled */
		}

		__svr_cur = NULL;
		s->inflight = NULL;
		__os_free(msg);
		if (cont == XTC_SVR_STOP) break;
		__svr_run_continue(s);   /* a handler may have armed a continuation */
	}

out:
	if (s->cb.terminate != NULL) s->cb.terminate(s->state, 0);
	if (!hooked)
		__svr_exited(s);
	/* A failed init ends the server with init's code, so a monitor or
	 * supervisor sees why it never served (xtc_svr_start returned
	 * before init ran).  s is gone once the hook ran: use the copy. */
	if (init_rc != XTC_OK && !XTC_DST_BUG(XTC_DST_BUG_INITEXIT))
		(void)xtc_exit_self(init_rc);
}

/* ----- public API ------------------------------------------------ */

int
xtc_svr_start(xtc_loop_t *loop, const xtc_svr_callbacks_t *cb, void *state,
              const xtc_svr_opts_t *opts, xtc_svr_t **out)
{
	struct xtc_svr *s;
	xtc_proc_opts_t pop = XTC_PROC_OPTS_INIT;
	int rc;
	xtc_pid_t pid;

	xtc_svr_opts_t o = XTC_SVR_OPTS_INIT;
	xtc_svr_callbacks_t c;

	if (loop == NULL || cb == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __xtc_sized_in(&c, NULL, sizeof c, cb, 0)) != XTC_OK)
		return rc;
	if (opts != NULL &&
	    (rc = __xtc_sized_in(&o, NULL, sizeof o, opts, 0)) != XTC_OK)
		return rc;
	/* A waiting start needs init to run while we wait. */
	if (o.wait_init && !__xtc_loop_may_wait(loop))
		return XTC_E_STATE;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK) return rc;
	s->loop = loop;
	s->cb = c;
	s->state = state;
	xtc_atomic_store_i32(&s->alive, 1, xtc_mo_relaxed);

	if ((rc = xtc_notify_create(&s->stopped)) != XTC_OK ||
	    (o.wait_init && (rc = xtc_notify_create(&s->started)) != XTC_OK)) {
		__svr_free(s);
		return rc;
	}

	pop.name = o.name;
	pop.mailbox_cap = o.mailbox_cap;
	pop.mailbox_watermark_pct = o.mailbox_watermark_pct;
	pop.mailbox_watermark_fn = o.mailbox_watermark_fn;
	pop.mailbox_watermark_user = o.mailbox_watermark_user;
	pop.sched_class = o.sched_class;
	rc = xtc_proc_spawn(loop, __svr_entry, s, &pop, &pid);
	if (rc != XTC_OK) {
		__svr_free(s);
		return rc;
	}
	xtc_atomic_pid_store(&s->pid, pid);
	if (o.wait_init) {
		/* init_rc is written before the signal (the notify's lock
		 * orders it).  A failed server is exiting on its own: orphan
		 * it, nobody joins. */
		if ((rc = __xtc_orc_start_wait(s->started, __svr_orphan, s)) ==
		    XTC_OK)
			rc = s->init_rc;
		if (rc != XTC_OK) {
			__svr_orphan(s);
			return rc;
		}
	}
	*out = s;
	return XTC_OK;
}

/* PUBLIC: void xtc_svr_child_main __P((void *)); */
void
xtc_svr_child_main(void *arg)
{
	xtc_svr_child_t sc = XTC_SVR_CHILD_INIT;
	struct xtc_svr *s;
	int rc;

	/* A bad spec arg ends the child with the error, which its
	 * supervisor sees (and a TRANSIENT spec retries until its budget). */
	rc = arg == NULL ? XTC_E_INVAL :
	                   __xtc_sized_in(&sc, NULL, sizeof sc, arg, 0);
	if (rc == XTC_OK && sc.cb == NULL)
		rc = XTC_E_INVAL;
	if (rc == XTC_OK &&
	    (rc = __os_calloc(1, sizeof *s, (void **)&s)) == XTC_OK &&
	    (rc = __xtc_sized_in(&s->cb, NULL, sizeof s->cb, sc.cb, 0)) !=
	        XTC_OK)
		__os_free(s);
	if (rc != XTC_OK) {
		(void)xtc_exit_self(rc);
		return;
	}
	s->loop = __xtc_proc_self_loop(NULL);
	s->state = sc.state;
	s->child = 1;
	xtc_atomic_store_i32(&s->alive, 1, xtc_mo_relaxed);
	__svr_entry(s);
}

int
xtc_svr_stop(xtc_svr_t *s)
{
	if (s == NULL) return XTC_E_INVAL;
	xtc_atomic_store_i32(&s->stop_requested, 1, xtc_mo_release);
	{
		uint8_t kick = 'S';
		(void)xtc_send(xtc_atomic_pid_load(&s->pid), &kick, 1);
	}
	return XTC_OK;
}

int
xtc_svr_join(xtc_svr_t *s, int64_t timeout_ns)
{
	int rc;
	if (s == NULL) return XTC_E_INVAL;
	rc = xtc_notify_wait(s->stopped, timeout_ns);
	/*
	 * Only reclaim the server if it actually stopped.  A finite
	 * timeout can expire while the server is still running its recv
	 * loop; freeing s then would be a use-after-free the moment the
	 * server next reads s->stop_requested (found by DST + ASan).
	 * On timeout, report XTC_E_AGAIN and leave
	 * s intact so the caller can join again (the server's async stop
	 * has not drained yet).  A blocking join (timeout_ns < 0) waits
	 * until the server signals, so it always reclaims.
	 */
	if (rc != XTC_OK)
		return rc;
	__svr_release(s);
	return XTC_OK;
}

xtc_pid_t
xtc_svr_pid(const xtc_svr_t *s)
{
	xtc_pid_t none = {0,0,0};
	return s ? xtc_atomic_pid_load(&s->pid) : none;
}

/* ----- client side ----------------------------------------------- */

/* Absolute deadline for timeout_ns (-1 = none), saturating at INT64_MAX
 * (a huge finite timeout must not overflow into "no deadline"). */
static int64_t
__svr_deadline(int64_t timeout_ns)
{
	int64_t now = 0;
	if (timeout_ns < 0) return -1;
	(void)__os_clock_mono(&now);
	return timeout_ns > INT64_MAX - now ? INT64_MAX : now + timeout_ns;
}

/* The next wait slice before `deadline`, or -1 once it has passed. */
static int64_t
__svr_slice(int64_t deadline, const xtc_abort_token_t *tok)
{
	int64_t now = 0, sl;
	sl = tok != NULL ? SVR_ABORT_SLICE_NS : SVR_LIVE_SLICE_NS;
	if (deadline < 0)
		return sl;
	(void)__os_clock_mono(&now);
	if (now >= deadline) return -1;
	return (deadline - now < sl) ? deadline - now : sl;
}

/* Off-proc wait on a registered slot.  XTC_OK when signalled (a reply,
 * or the server's exit: the caller re-reads slot->replied and
 * slot->gone under its bucket lock either way). */
static int
__svr_wait_slot(struct __svr_reply_slot *slot, xtc_pid_t target,
                int64_t timeout_ns, xtc_abort_token_t *tok)
{
	int64_t deadline = __svr_deadline(timeout_ns), w;
	int rc;

	for (;;) {
		if (tok != NULL && xtc_abort_token_is_aborted(tok))
			return XTC_E_ABORTED;
		w = __svr_slice(deadline, tok);
		rc = xtc_notify_wait(slot->done, w < 0 ? 0 : w);
		if (rc != XTC_E_AGAIN) return rc;
		if (w < 0) return XTC_E_AGAIN;           /* timed out */
		if (__svr_gone(target)) return XTC_E_NOTFOUND;
	}
}

/*
 * Internal call implementation shared by xtc_svr_call (tok == NULL)
 * and xtc_svr_call_abortable (tok != NULL).  XTC_E_ABORTED if the token
 * fires first, XTC_E_NOTFOUND if the server exits without replying,
 * noticed at once: an in-proc caller monitors the server for the call
 * (and demonitors with flush after the reply), a plain thread is woken by
 * the server's exit hook.  (Before 1.55.1 a server that died mid-call
 * left an infinite-timeout caller parked forever; through 2.2 the death
 * was found by a 25 ms liveness poll.)  A timeout, abort or server exit
 * only stops the CALLER waiting.  A later reply to an off-proc caller is
 * discarded; to an in-proc caller it lands in its mailbox and is dropped
 * by that proc's next in-proc call (see __recv_reply_for_tag).
 */
static int
__svr_call(xtc_pid_t target, const void *req, size_t req_size,
           void **out_reply, size_t *out_size, int64_t timeout_ns,
           xtc_abort_token_t *tok)
{
	struct __svr_reply_slot slot;
	struct __svr_slot_bucket *b;
	uint8_t  *buf = NULL;
	size_t    msg_size;
	int       rc;

	if (req_size > 0 && req == NULL) return XTC_E_INVAL;
	/* Guard against size_t overflow in the framed-message size.
	 * Every path below computes msg_size = header + req_size; a
	 * req_size near SIZE_MAX would wrap to a small allocation and
	 * the subsequent memcpy would overflow the heap.  The largest
	 * header used below is SVR_PCALL_HDR bytes; reject with margin. */
	if (req_size > SIZE_MAX - 64) return XTC_E_INVAL;
	if (out_reply == NULL || out_size == NULL) return XTC_E_INVAL;

	/* Route based on whether we're a proc or a plain thread.  In-proc
	 * callers can't safely block the loop on a notify (it would
	 * starve the server itself), so route the reply back through
	 * the caller's mailbox using xtc_recv_match. */
	if (!xtc_pid_is_none(xtc_self())) {
		static uint64_t g_next_tag;
		uint64_t tag =
		    xtc_atomic_fetch_add_u64(&g_next_tag, 1, xtc_mo_relaxed) +
		    1;
		xtc_pid_t self_pid = xtc_self();
		uint64_t mref = 0;

		msg_size = SVR_PCALL_HDR + req_size;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK)
			return XTC_E_NOMEM;
		buf[0] = 'C';
		buf[1] = 'p';
		memcpy(buf + 2, &self_pid, sizeof self_pid);
		memcpy(buf + 2 + sizeof self_pid, &tag, 8);
		if (req_size > 0) memcpy(buf + SVR_PCALL_HDR, req, req_size);

		/* Monitor the server for the call: its DOWN ends the wait. */
		if ((rc = xtc_monitor(target, &mref)) != XTC_OK) {
			__os_free(buf);
			return rc;
		}
		/* Move the frame (no second copy); ours again on error. */
		rc = xtc_send_owned(target, buf, msg_size);
		if (rc == XTC_OK)
			rc = __recv_reply_for_tag(tag, mref, target, out_reply,
			    out_size, timeout_ns, tok);
		else
			__os_free(buf);
		(void)xtc_demonitor(mref, XTC_DEMONITOR_FLUSH);
		return rc;
	}

	memset(&slot, 0, sizeof slot);
	if ((rc = xtc_notify_create(&slot.done)) != XTC_OK)
		return rc;
	msg_size = 10 + req_size;
	if (__os_malloc(msg_size, (void **)&buf) != XTC_OK) {
		xtc_notify_destroy(slot.done);
		return XTC_E_NOMEM;
	}
	slot.id = xtc_atomic_fetch_add_u64(&__svr_slot_seq, 1, xtc_mo_relaxed) +
	    1; /* never 0 (the pid path) */
	slot.target = target;
	b = __svr_slot_bucket(slot.id);
	(void)__xtc_mtx_lock(&b->lock);
	slot.next = b->head;
	b->head = &slot;
	(void)__xtc_mtx_unlock(&b->lock);

	buf[0] = 'C';
	buf[1] = 's';
	memcpy(buf + 2, &slot.id, 8);
	if (req_size > 0) memcpy(buf + 10, req, req_size);
	rc = xtc_send_owned(target, buf, msg_size);
	if (rc != XTC_OK)
		__os_free(buf);
	if (rc == XTC_OK)
		rc = __svr_wait_slot(&slot, target, timeout_ns, tok);

	/* Unlink before the frame dies; from here the server cannot reach
	 * the slot.  A reply that landed after the wait gave up still
	 * wins: return it rather than drop it. */
	(void)__xtc_mtx_lock(&b->lock);
	__svr_slot_unlink_locked(b, &slot);
	if (slot.replied) {
		*out_reply = slot.data;
		*out_size = slot.size;
		rc = XTC_OK;
	} else if (slot.gone && rc == XTC_OK) {
		rc = XTC_E_NOTFOUND; /* woken by the server's exit */
	}
	(void)__xtc_mtx_unlock(&b->lock);
	xtc_notify_destroy(slot.done);
	return rc;
}

/* PUBLIC: int xtc_svr_call __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t)); */
int
xtc_svr_call(xtc_pid_t target, const void *req, size_t req_size,
             void **out_reply, size_t *out_size, int64_t timeout_ns)
{
	return __svr_call(target, req, req_size, out_reply, out_size,
	    timeout_ns, NULL);
}

/* PUBLIC: int xtc_svr_call_abortable __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t, xtc_abort_token_t *)); */
int
xtc_svr_call_abortable(xtc_pid_t target, const void *req, size_t req_size,
                       void **out_reply, size_t *out_size,
                       int64_t timeout_ns, xtc_abort_token_t *tok)
{
	return __svr_call(target, req, req_size, out_reply, out_size,
	    timeout_ns, tok);
}

int
xtc_svr_cast(xtc_pid_t target, const void *msg, size_t size)
{
	uint8_t *buf;
	int rc;
	if (size > 0 && msg == NULL) return XTC_E_INVAL;
	/* Overflow guard: size + 1 must not wrap (see xtc_svr_call). */
	if (size > SIZE_MAX - 1) return XTC_E_INVAL;
	buf = NULL;
	if (__os_malloc(size + 1, (void **)&buf) != XTC_OK) return XTC_E_NOMEM;
	buf[0] = 'X';
	if (size > 0) memcpy(buf + 1, msg, size);
	rc = xtc_send_owned(target, buf, size + 1);
	if (rc != XTC_OK)
		__os_free(buf);
	return rc;
}

/* PUBLIC: int xtc_svr_continue __P((void *)); */
int
xtc_svr_continue(void *cont)
{
	/* Arm a continuation to run before the server's next recv.  Must be
	 * called from within a server callback (init / handle_*), which runs
	 * on the server's own loop thread; __svr_cur identifies it. */
	struct xtc_svr *s = __svr_cur;
	if (s == NULL) return XTC_E_INVAL;
	s->cont_pending = 1;
	s->cont_arg = cont;
	return XTC_OK;
}

int
xtc_svr_reply(xtc_svr_call_t *call, const void *reply, size_t size)
{
	int rc = XTC_E_INVAL;

	if (call == NULL) return XTC_E_INVAL;

	/* Fault-injection: force the reply-copy allocation to fail on the
	 * non-empty-payload path, whichever branch (slot or in-proc
	 * reply_pid) this call uses.  Covers the XTC_E_NOMEM edge in both. */
	if (size > 0 && xtc_inject_check("svr.reply.oom")) {
		xtc_inject_trigger("svr.reply.oom");
		return XTC_E_NOMEM;
	}

	if (call->slot_id != 0) {
		struct __svr_slot_bucket *b = __svr_slot_bucket(call->slot_id);
		struct __svr_reply_slot *slot;
		void *copy = NULL;
		if (size > 0) {
			/* Handed to the xtc_svr_call caller as *out_reply, which
			 * the contract says to release with xtc_free (== __os_free
			 * == the installed alloc hook).  It MUST therefore be
			 * allocated with __os_malloc, or an embedder with a custom
			 * allocator (e.g. PostgreSQL) frees a libc-malloc'd pointer
			 * with the hook's free -- a mismatched free / heap
			 * corruption. */
			if (__os_malloc(size, &copy) != XTC_OK)
				return XTC_E_NOMEM;
			memcpy(copy, reply, size);
		}
		/* The caller may have stopped waiting (timeout, abort, its
		 * server-gone check) and unlinked its slot: then there is
		 * nobody to deliver to and the reply is discarded. */
		rc = XTC_E_NOTFOUND;
		(void)__xtc_mtx_lock(&b->lock);
		for (slot = b->head; slot != NULL; slot = slot->next)
			if (slot->id == call->slot_id)
				break;
		if (slot != NULL && !slot->replied) {
			slot->data = copy;
			slot->size = size;
			slot->replied = 1;
			copy = NULL;
			(void)xtc_notify_signal(slot->done);
			rc = XTC_OK;
		}
		(void)__xtc_mtx_unlock(&b->lock);
		if (copy != NULL) __os_free(copy);
	} else if (!xtc_pid_is_none(call->reply_pid)) {
		/* Encode reply for in-proc caller: payload + tag + magic. */
		uint8_t *buf;
		size_t msg_size;
		if (size > SIZE_MAX - SVR_REPLY_HDR) return XTC_E_INVAL;
		msg_size = SVR_REPLY_HDR + size;
		buf = NULL;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK)
			return XTC_E_NOMEM;
		if (size > 0) memcpy(buf, reply, size);
		memcpy(buf + size, &call->reply_tag, 8);
		memcpy(buf + size + 8, SVR_REPLY_MAGIC, 8);
		rc = xtc_send_owned(call->reply_pid, buf, msg_size);
		if (rc != XTC_OK)
			__os_free(buf);
	}

	/* A handle from xtc_svr_call_save outlived its handle_call and is
	 * owned by the caller; free it once replied. */
	if (call->heap)
		__os_free(call);
	return rc;
}

/* PUBLIC: xtc_svr_call_t *xtc_svr_call_save __P((const xtc_svr_call_t *)); */
xtc_svr_call_t *
xtc_svr_call_save(const xtc_svr_call_t *call)
{
	struct xtc_svr_call *saved;

	if (call == NULL) return NULL;
	if (__os_malloc(sizeof *saved, (void **)&saved) != XTC_OK) return NULL;
	*saved = *call;
	saved->heap = 1;
	return saved;
}

/* In-proc receive helper.  The predicate takes ANY svr reply (magic
 * suffix) and the DOWN of this call's monitor of the server (u: the
 * ref, or NULL for replies only); other messages stay queued for the
 * proc.  A proc has at most one in-proc call outstanding (the caller
 * waits here), so a reply whose tag is not the current one answers an
 * earlier call that already gave up (timeout, abort, server exit): it is
 * dropped here, so late replies never pile up in the save queue or slow
 * later calls. */
static int
__svr_is_reply(const void *data, size_t size)
{
	return size >= SVR_REPLY_HDR &&
	    memcmp((const uint8_t *)data + size - sizeof SVR_REPLY_MAGIC,
	    SVR_REPLY_MAGIC, sizeof SVR_REPLY_MAGIC) == 0;
}

static int
__match_svr_reply(const void *data, size_t size, void *u)
{
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	if (__svr_is_reply(data, size))
		return 1;
	return u != NULL && size > 0 && *(const uint8_t *)data == 'D' &&
	    xtc_down_decode_ex(data, size, &di) == XTC_OK &&
	    di.ref == *(const uint64_t *)u;
}

static int
__recv_reply_for_tag(uint64_t tag, uint64_t mref, xtc_pid_t target, void **out,
    size_t *out_size, int64_t timeout_ns, xtc_abort_token_t *tok)
{
	void  *msg = NULL;
	size_t size = 0;
	int64_t deadline = __svr_deadline(timeout_ns), w;
	uint64_t got;
	int    rc, gone = 0;

	/* The server's DOWN (mref) ends the wait.  A reply the server sent
	 * before exiting is queued ahead of its DOWN (both are delivered
	 * synchronously from the server's fiber, in that order), so once the
	 * DOWN is seen one non-blocking look takes such a reply, and then the
	 * call is XTC_E_NOTFOUND.  Under the simulator a cross-loop delivery
	 * can be deferred by a seeded latency drawn per message, so a DOWN
	 * can overtake the reply: there one more slice is allowed for it.
	 * Sliced otherwise only for the abort token and the liveness backstop
	 * (SVR_LIVE_SLICE_NS). */
	for (;;) {
		if (tok != NULL && xtc_abort_token_is_aborted(tok))
			return XTC_E_ABORTED;
		w = !gone              ? __svr_slice(deadline, tok) :
		    __xtc_sim_active() ? SVR_ABORT_SLICE_NS :
		                         0;
		rc = xtc_recv_match(__match_svr_reply, gone ? NULL : &mref,
		    &msg, &size, w < 0 ? 0 : w);
		if (rc == XTC_OK) {
			if (!__svr_is_reply(msg, size)) {
				__os_free(msg); /* the server's DOWN */
				msg = NULL;
				gone = 1;
				continue;
			}
			memcpy(&got, (uint8_t *)msg + size - SVR_REPLY_HDR,
			    sizeof got);
			if (got == tag) break;
			__os_free(msg);                /* late reply: drop */
			msg = NULL;
			continue;
		}
		if (rc != XTC_E_AGAIN) return rc;
		if (gone) return XTC_E_NOTFOUND;
		if (w < 0) return XTC_E_AGAIN;         /* timed out */
		if (__svr_gone(target)) gone = 1;
	}
	/* Drop the trailing header: the caller gets the received block
	 * itself (an xtc_free-able __os_malloc block), no copy. */
	*out_size = size - SVR_REPLY_HDR;
	if (*out_size > 0) {
		*out = msg;
	} else {
		*out = NULL;
		__os_free(msg);
	}
	return XTC_OK;
}
