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
 *	                            reply slot (see __svr_slots below)
 *	  'C' 'p' pid[8] tag[8] payload -- in-proc caller: reply by mailbox
 *
 *	In-proc reply (to the caller's mailbox):
 *	  SVR_REPLY_MAGIC[8] tag[8] payload
 *	The magic keeps an ordinary application message from being taken
 *	as a reply; the 64-bit tag is never reused.
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
#include "xtc_sim.h"       /* XTC_SIM_BUGGIFY: DST pessimal-path injection */
#include "xtc_svr.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "xtc_async.h"    /* xtc_yield: buggify delay-dispatch site */
#include "xtc_inject.h"   /* svr.reply.oom fault-injection point */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static int __recv_reply_for_tag(uint64_t tag, xtc_pid_t target, void **out,
                                size_t *out_size, int64_t timeout_ns,
                                xtc_abort_token_t *tok);

#define SVR_REPLY_HDR 16
static const uint8_t SVR_REPLY_MAGIC[8] = {
	0xfe, 'x', 't', 'c', 's', 'v', 'r', 0xa5
};

/* xtc_pid_t packed into one word so it can be published atomically. */
static uint64_t
__svr_pid_pack(xtc_pid_t p)
{
	uint64_t v;
	memcpy(&v, &p, sizeof v);
	return v;
}

static xtc_pid_t
__svr_pid_unpack(uint64_t v)
{
	xtc_pid_t p;
	memcpy(&p, &v, sizeof p);
	return p;
}

struct xtc_svr {
	xtc_loop_t           *loop;
	xtc_svr_callbacks_t   cb;
	void                 *state;
	/* Packed xtc_pid_t. Stored by xtc_svr_start and again (same value)
	 * by the server proc itself, read from any thread. */
	_Atomic uint64_t      pid;
	xtc_notify_t         *stopped;
	_Atomic int           stop_requested;
	_Atomic int           alive;
	/* handle_continue: a callback may arm a continuation via
	 * xtc_svr_continue(); the server runs handle_continue(state, cont)
	 * before its next recv, so it can finish expensive init/post-work
	 * off the caller's critical path, race-free. */
	int                   cont_pending;
	void                 *cont_arg;
	void                 *inflight;   /* message being dispatched */
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
 * caller's stack and is reachable ONLY through __svr_slots, by the id
 * the request carries -- never by a pointer in the message.  Every
 * access (reply store, signal, caller unlink) happens under
 * __svr_slots_lock, and the caller unlinks before it returns, so a reply
 * that arrives after a timeout finds no slot and is discarded.  (Before
 * 1.55.1 the message carried &slot and a late reply wrote into the dead
 * frame: ASan stack-use-after-return / SEGV in xtc_notify_signal.)
 * The list holds one entry per thread blocked in a call, so a linear
 * lookup is fine.
 */
struct __svr_reply_slot {
	struct __svr_reply_slot *next;
	uint64_t        id;
	xtc_notify_t   *done;
	void           *data;
	size_t          size;
	int             replied;
};

static pthread_mutex_t          __svr_slots_lock = PTHREAD_MUTEX_INITIALIZER;
static struct __svr_reply_slot *__svr_slots;
static uint64_t                 __svr_slot_seq;

struct xtc_svr_call {
	struct xtc_svr           *svr;
	uint64_t                  slot_id;     /* slot path (0 = pid path) */
	xtc_pid_t                 reply_pid;   /* pid-based path  */
	uint64_t                  reply_tag;
	int                       heap;        /* 1 == heap copy from _save */
};

/* Slice length for reply waits: the abort token and the server's
 * liveness are re-checked between slices. */
#define SVR_WAIT_SLICE_NS (25LL * 1000 * 1000)

/* Has the server exited?  Resolves the pid the same way xtc_send does,
 * from a proc or a plain thread. */
static int
__svr_gone(xtc_pid_t target)
{
	xtc_mailbox_stats_t st;
	return xtc_proc_mailbox_stats(target, &st) != XTC_OK;
}

static void
__svr_slot_unlink_locked(struct __svr_reply_slot *slot)
{
	struct __svr_reply_slot **pp;
	for (pp = &__svr_slots; *pp != NULL; pp = &(*pp)->next)
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
	    !atomic_load_explicit(&s->stop_requested, memory_order_acquire)) {
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
	atomic_store_explicit(&s->alive, 0, memory_order_release);
	(void)xtc_notify_signal(s->stopped);   /* s may be freed after this */
}

static void
__svr_entry(void *arg)
{
	struct xtc_svr *s = arg;
	int hooked;
	atomic_store_explicit(&s->pid, __svr_pid_pack(xtc_self()),
	    memory_order_release);
	hooked = (xtc_proc_at_exit(__svr_exited, s) == XTC_OK);

	if (s->cb.init != NULL) {
		int rc;
		__svr_cur = s;
		rc = s->cb.init(s->state);
		__svr_cur = NULL;
		if (rc != XTC_OK) goto out;
	}
	__svr_run_continue(s);   /* init may have armed a continuation */

	while (!atomic_load_explicit(&s->stop_requested, memory_order_acquire)) {
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
			 *   'p' = pid  routing  (bytes 2..9 = xtc_pid_t,
			 *                       bytes 10..17 = tag,        payload at +18)
			 */
			struct xtc_svr_call call = {0};
			size_t hdr;
			call.svr = s;
			__svr_cur = s;   /* for xtc_svr_continue() during the callback */
			if (size >= 10 && ((uint8_t *)msg)[1] == 's') {
				memcpy(&call.slot_id, (uint8_t *)msg + 2, 8);
				hdr = 10;
			} else if (size >= 18 && ((uint8_t *)msg)[1] == 'p') {
				memcpy(&call.reply_pid, (uint8_t *)msg + 2, 8);
				memcpy(&call.reply_tag, (uint8_t *)msg + 10, 8);
				hdr = 18;
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
}

/* ----- public API ------------------------------------------------ */

int
xtc_svr_start(xtc_loop_t *loop, const xtc_svr_callbacks_t *cb, void *state,
              const xtc_svr_opts_t *opts, xtc_svr_t **out)
{
	struct xtc_svr *s;
	xtc_proc_opts_t pop = {0};
	int rc;
	xtc_pid_t pid;

	if (loop == NULL || cb == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK) return rc;
	s->loop = loop;
	s->cb = *cb;
	s->state = state;
	atomic_store_explicit(&s->alive, 1, memory_order_relaxed);

	if ((rc = xtc_notify_create(&s->stopped)) != XTC_OK) {
		__os_free(s);
		return rc;
	}

	if (opts != NULL) {
		pop.name = opts->name;
		pop.mailbox_cap = opts->mailbox_cap;
	}
	rc = xtc_proc_spawn(loop, __svr_entry, s, &pop, &pid);
	if (rc != XTC_OK) {
		xtc_notify_destroy(s->stopped);
		__os_free(s);
		return rc;
	}
	atomic_store_explicit(&s->pid, __svr_pid_pack(pid),
	    memory_order_release);
	*out = s;
	return XTC_OK;
}

int
xtc_svr_stop(xtc_svr_t *s)
{
	if (s == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&s->stop_requested, 1, memory_order_release);
	{
		uint8_t kick = 'S';
		(void)xtc_send(__svr_pid_unpack(atomic_load_explicit(&s->pid,
		    memory_order_acquire)), &kick, 1);
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
	xtc_notify_destroy(s->stopped);
	__os_free(s);
	return XTC_OK;
}

xtc_pid_t
xtc_svr_pid(const xtc_svr_t *s)
{
	xtc_pid_t none = {0,0,0};
	return s ? __svr_pid_unpack(atomic_load_explicit(&s->pid,
	               memory_order_acquire)) : none;
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
__svr_slice(int64_t deadline)
{
	int64_t now = 0;
	if (deadline < 0) return SVR_WAIT_SLICE_NS;
	(void)__os_clock_mono(&now);
	if (now >= deadline) return -1;
	return (deadline - now < SVR_WAIT_SLICE_NS) ? deadline - now
	                                            : SVR_WAIT_SLICE_NS;
}

/* Off-proc wait on a registered slot.  XTC_OK when signalled; the
 * caller re-reads slot->replied under __svr_slots_lock either way. */
static int
__svr_wait_slot(struct __svr_reply_slot *slot, xtc_pid_t target,
                int64_t timeout_ns, xtc_abort_token_t *tok)
{
	int64_t deadline = __svr_deadline(timeout_ns), w;
	int rc;

	for (;;) {
		if (tok != NULL && xtc_abort_token_is_aborted(tok))
			return XTC_E_ABORTED;
		w = __svr_slice(deadline);
		rc = xtc_notify_wait(slot->done, w < 0 ? 0 : w);
		if (rc != XTC_E_AGAIN) return rc;
		if (w < 0) return XTC_E_AGAIN;           /* timed out */
		if (__svr_gone(target)) return XTC_E_NOTFOUND;
	}
}

/*
 * Internal call implementation shared by xtc_svr_call (tok == NULL)
 * and xtc_svr_call_abortable (tok != NULL).  The reply wait is sliced
 * (SVR_WAIT_SLICE_NS) so the abort token and the server's liveness are
 * re-checked while waiting: XTC_E_ABORTED if the token fires first,
 * XTC_E_NOTFOUND if the server exits without replying (before 1.55.1 a
 * server that died mid-call left an infinite-timeout caller parked
 * forever).  A timeout, abort or server exit only stops the CALLER
 * waiting.  A later reply to an off-proc caller is discarded; to an
 * in-proc caller it lands in its mailbox and is dropped by that proc's
 * next in-proc call (see __recv_reply_for_tag).
 */
static int
__svr_call(xtc_pid_t target, const void *req, size_t req_size,
           void **out_reply, size_t *out_size, int64_t timeout_ns,
           xtc_abort_token_t *tok)
{
	struct __svr_reply_slot slot;
	uint8_t  *buf = NULL;
	size_t    msg_size;
	int       rc;

	if (req_size > 0 && req == NULL) return XTC_E_INVAL;
	/* Guard against size_t overflow in the framed-message size.
	 * Every path below computes msg_size = header + req_size; a
	 * req_size near SIZE_MAX would wrap to a small allocation and
	 * the subsequent memcpy would overflow the heap.  The largest
	 * header used below is 18 bytes; reject with margin. */
	if (req_size > SIZE_MAX - 64) return XTC_E_INVAL;
	if (out_reply == NULL || out_size == NULL) return XTC_E_INVAL;

	/* Route based on whether we're a proc or a plain thread.  In-proc
	 * callers can't safely block the loop on a notify (it would
	 * starve the server itself), so route the reply back through
	 * the caller's mailbox using xtc_recv_match. */
	if (!xtc_pid_is_none(xtc_self())) {
		static _Atomic uint64_t g_next_tag;
		uint64_t tag = atomic_fetch_add_explicit(&g_next_tag, 1,
		    memory_order_relaxed) + 1;
		xtc_pid_t self_pid = xtc_self();

		msg_size = 18 + req_size;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK)
			return XTC_E_NOMEM;
		buf[0] = 'C';
		buf[1] = 'p';
		memcpy(buf + 2, &self_pid, 8);
		memcpy(buf + 10, &tag, 8);
		if (req_size > 0) memcpy(buf + 18, req, req_size);

		rc = xtc_send(target, buf, msg_size);
		__os_free(buf);
		if (rc != XTC_OK) return rc;

		return __recv_reply_for_tag(tag, target, out_reply, out_size,
		    timeout_ns, tok);
	}

	memset(&slot, 0, sizeof slot);
	if ((rc = xtc_notify_create(&slot.done)) != XTC_OK)
		return rc;
	msg_size = 10 + req_size;
	if (__os_malloc(msg_size, (void **)&buf) != XTC_OK) {
		xtc_notify_destroy(slot.done);
		return XTC_E_NOMEM;
	}
	(void)__xtc_mtx_lock(&__svr_slots_lock);
	slot.id = ++__svr_slot_seq;
	slot.next = __svr_slots;
	__svr_slots = &slot;
	(void)__xtc_mtx_unlock(&__svr_slots_lock);

	buf[0] = 'C';
	buf[1] = 's';
	memcpy(buf + 2, &slot.id, 8);
	if (req_size > 0) memcpy(buf + 10, req, req_size);
	rc = xtc_send(target, buf, msg_size);
	__os_free(buf);
	if (rc == XTC_OK)
		rc = __svr_wait_slot(&slot, target, timeout_ns, tok);

	/* Unlink before the frame dies; from here the server cannot reach
	 * the slot.  A reply that landed after the wait gave up still
	 * wins: return it rather than drop it. */
	(void)__xtc_mtx_lock(&__svr_slots_lock);
	__svr_slot_unlink_locked(&slot);
	if (slot.replied) {
		*out_reply = slot.data;
		*out_size = slot.size;
		rc = XTC_OK;
	}
	(void)__xtc_mtx_unlock(&__svr_slots_lock);
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
	rc = xtc_send(target, buf, size + 1);
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
		(void)__xtc_mtx_lock(&__svr_slots_lock);
		for (slot = __svr_slots; slot != NULL; slot = slot->next)
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
		(void)__xtc_mtx_unlock(&__svr_slots_lock);
		if (copy != NULL) __os_free(copy);
	} else if (!xtc_pid_is_none(call->reply_pid)) {
		/* Encode reply for in-proc caller: magic + tag + payload. */
		uint8_t *buf;
		size_t msg_size;
		if (size > SIZE_MAX - SVR_REPLY_HDR) return XTC_E_INVAL;
		msg_size = SVR_REPLY_HDR + size;
		buf = NULL;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK)
			return XTC_E_NOMEM;
		memcpy(buf, SVR_REPLY_MAGIC, 8);
		memcpy(buf + 8, &call->reply_tag, 8);
		if (size > 0) memcpy(buf + SVR_REPLY_HDR, reply, size);
		rc = xtc_send(call->reply_pid, buf, msg_size);
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
 * prefix); other messages stay queued for the proc.  A proc has at most
 * one in-proc call outstanding (the caller waits here), so a reply whose
 * tag is not the current one answers an earlier call that already gave
 * up (timeout, abort, server exit): it is dropped here, so late replies
 * never pile up in the save queue or slow later calls. */
static int
__match_svr_reply(const void *data, size_t size, void *u)
{
	(void)u;
	return size >= SVR_REPLY_HDR &&
	    memcmp(data, SVR_REPLY_MAGIC, sizeof SVR_REPLY_MAGIC) == 0;
}

static int
__recv_reply_for_tag(uint64_t tag, xtc_pid_t target, void **out,
                     size_t *out_size, int64_t timeout_ns,
                     xtc_abort_token_t *tok)
{
	void  *msg = NULL;
	size_t size = 0;
	int64_t deadline = __svr_deadline(timeout_ns), w;
	uint64_t got;
	int    rc, gone = 0;

	/* Sliced so the abort token and the server's liveness are polled
	 * between mailbox waits (see __svr_call).  Once the server is seen
	 * gone, one more slice is allowed for a reply sent just before the
	 * exit and still in flight (cross-loop delivery can be deferred). */
	for (;;) {
		if (tok != NULL && xtc_abort_token_is_aborted(tok))
			return XTC_E_ABORTED;
		w = __svr_slice(deadline);
		rc = xtc_recv_match(__match_svr_reply, NULL, &msg, &size,
		    w < 0 ? 0 : w);
		if (rc == XTC_OK) {
			memcpy(&got, (uint8_t *)msg + 8, sizeof got);
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
	/* Strip the header. */
	*out_size = size - SVR_REPLY_HDR;
	if (*out_size > 0) {
		void *copy = NULL;
		if (__os_malloc(*out_size, &copy) != XTC_OK) {
			__os_free(msg); return XTC_E_NOMEM;
		}
		memcpy(copy, (uint8_t *)msg + SVR_REPLY_HDR, *out_size);
		*out = copy;
	} else {
		*out = NULL;
	}
	__os_free(msg);
	return XTC_OK;
}
