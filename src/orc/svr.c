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
 *	For a call:
 *	  byte 0:    'C'
 *	  bytes 1..8: reply-channel pointer (xtc_chan_oneshot_t *)
 *	             encoded as little-endian uint64 -- used by reply().
 *	  bytes 9..: user payload
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

static int __recv_reply_for_tag(uint32_t tag, void **out, size_t *out_size,
                                int64_t timeout_ns, xtc_abort_token_t *tok);

struct xtc_svr {
	xtc_loop_t           *loop;
	xtc_svr_callbacks_t   cb;
	void                 *state;
	xtc_pid_t             pid;
	xtc_notify_t         *stopped;
	_Atomic int           stop_requested;
	_Atomic int           alive;
	/* handle_continue: a callback may arm a continuation via
	 * xtc_svr_continue(); the server runs handle_continue(state, cont)
	 * before its next recv, so it can finish expensive init/post-work
	 * off the caller's critical path, race-free. */
	int                   cont_pending;
	void                 *cont_arg;
};

/* The server whose callback is currently running on this thread, so
 * xtc_svr_continue() can find it.  A server proc runs its callbacks
 * only on its own loop thread, so a plain TLS pointer is correct. */
static XTC_THREAD_LOCAL struct xtc_svr *__svr_cur;

/* Reply slot -- owned by the caller of xtc_svr_call.  Lives on the
 * caller's stack (or heap); the server-side reply path accesses it
 * via the pointer encoded in the call message. */
struct __svr_reply_slot {
	pthread_mutex_t lock;
	xtc_notify_t   *done;
	void           *data;
	size_t          size;
	int             rc;
};

struct xtc_svr_call {
	struct xtc_svr           *svr;
	struct __svr_reply_slot  *slot;        /* slot-based path */
	xtc_pid_t                 reply_pid;   /* pid-based path  */
	uint32_t                  reply_tag;
	int                       heap;        /* 1 == heap copy from _save */
};

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

static void
__svr_entry(void *arg)
{
	struct xtc_svr *s = arg;
	s->pid = xtc_self();

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

		/* Internal stop-kick: a single 'S' byte sent by xtc_svr_stop
		 * to wake us from the recv-poll.  Don't dispatch. */
		if (size == 1 && kind == 'S') {
			__os_free(msg);
			continue;
		}

		if (kind == 'C') {
			/* Call: byte 1 is the routing tag.
			 *   's' = slot routing  (bytes 2..9 = slot ptr,    payload at +10)
			 *   'p' = pid  routing  (bytes 2..9 = xtc_pid_t,
			 *                       bytes 10..13 = tag,        payload at +14)
			 */
			struct xtc_svr_call call = {0};
			call.svr = s;
			__svr_cur = s;   /* for xtc_svr_continue() during the callback */
			if (size >= 10 && ((uint8_t *)msg)[1] == 's') {
				uint64_t enc = 0;
				int i;
				for (i = 0; i < 8; i++)
					enc |= (uint64_t)(((uint8_t *)msg)[2 + i]) << (8 * i);
				call.slot = (struct __svr_reply_slot *)(uintptr_t)enc;
			} else if (size >= 14 && ((uint8_t *)msg)[1] == 'p') {
				memcpy(&call.reply_pid, (uint8_t *)msg + 2, 8);
				memcpy(&call.reply_tag, (uint8_t *)msg + 10, 4);
			} else {
				/* Malformed call.  Skip. */
				__os_free(msg);
				continue;
			}
			{
				size_t hdr = (call.slot != NULL) ? 10 : 14;
				if (s->cb.handle_call != NULL) {
					cont = s->cb.handle_call(s->state,
					    (uint8_t *)msg + hdr,
					    size - hdr,
					    &call);
				} else {
					/* No handler: send empty reply. */
					(void)xtc_svr_reply(&call, NULL, 0);
				}
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
		__os_free(msg);
		if (cont == XTC_SVR_STOP) break;
		__svr_run_continue(s);   /* a handler may have armed a continuation */
	}

out:
	if (s->cb.terminate != NULL) s->cb.terminate(s->state, 0);
	atomic_store_explicit(&s->alive, 0, memory_order_release);
	(void)xtc_notify_signal(s->stopped);
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
	s->pid = pid;
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
		(void)xtc_send(s->pid, &kick, 1);
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
	return s ? s->pid : none;
}

/* ----- client side ----------------------------------------------- */

/*
 * Internal call implementation shared by xtc_svr_call (tok == NULL)
 * and xtc_svr_call_abortable (tok != NULL).  When a token is present
 * the reply wait is sliced so the token is polled while waiting, and
 * the call returns XTC_E_ABORTED if the token fires before the reply
 * arrives.  Cancellation only stops the CALLER waiting; the server's
 * handler keeps running -- a late reply is discarded (the caller is
 * typically being torn down on cancel).
 */
static int
__svr_call(xtc_pid_t target, const void *req, size_t req_size,
           void **out_reply, size_t *out_size, int64_t timeout_ns,
           xtc_abort_token_t *tok)
{
	if (req_size > 0 && req == NULL) return XTC_E_INVAL;
	/* Guard against size_t overflow in the framed-message size.
	 * Every path below computes msg_size = header + req_size; a
	 * req_size near SIZE_MAX would wrap to a small allocation and
	 * the subsequent memcpy would overflow the heap.  The largest
	 * header used below is 14 bytes; reject with margin. */
	if (req_size > SIZE_MAX - 64) return XTC_E_INVAL;
	if (out_reply == NULL || out_size == NULL) return XTC_E_INVAL;

	/* Route based on whether we're a proc or a plain thread.  In-proc
	 * callers can't safely block the loop on a notify (it would
	 * starve the server itself), so route the reply back through
	 * the caller's mailbox using xtc_recv_match. */
	if (!xtc_pid_is_none(xtc_self())) {
		static _Atomic uint32_t g_next_tag;
		uint32_t tag = atomic_fetch_add_explicit(&g_next_tag, 1,
		    memory_order_relaxed) + 1;
		xtc_pid_t self_pid = xtc_self();
		uint8_t  *buf;
		size_t    msg_size = 14 + req_size;
		int       rc;

		buf = NULL;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK)
			return XTC_E_NOMEM;
		buf[0] = 'C';
		buf[1] = 'p';
		memcpy(buf + 2, &self_pid, 8);
		memcpy(buf + 10, &tag, 4);
		if (req_size > 0) memcpy(buf + 14, req, req_size);

		rc = xtc_send(target, buf, msg_size);
		__os_free(buf);
		if (rc != XTC_OK) return rc;

		return __recv_reply_for_tag(tag, out_reply, out_size,
		    timeout_ns, tok);
	} else {
		struct __svr_reply_slot slot;
		uint8_t  *buf;
		size_t    msg_size;
		int       rc;
		uint64_t  enc;
		int       i;

		memset(&slot, 0, sizeof slot);
		(void)pthread_mutex_init(&slot.lock, NULL);
		if ((rc = xtc_notify_create(&slot.done)) != XTC_OK) {
			(void)pthread_mutex_destroy(&slot.lock);
			return rc;
		}
		slot.rc = XTC_E_AGAIN;

		msg_size = 10 + req_size;
		buf = NULL;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK) {
			xtc_notify_destroy(slot.done);
			(void)pthread_mutex_destroy(&slot.lock);
			return XTC_E_NOMEM;
		}
		buf[0] = 'C';
		buf[1] = 's';
		enc = (uint64_t)(uintptr_t)&slot;
		for (i = 0; i < 8; i++)
			buf[2 + i] = (uint8_t)((enc >> (8 * i)) & 0xff);
		if (req_size > 0) memcpy(buf + 10, req, req_size);

		rc = xtc_send(target, buf, msg_size);
		__os_free(buf);
		if (rc != XTC_OK) {
			xtc_notify_destroy(slot.done);
			(void)pthread_mutex_destroy(&slot.lock);
			return rc;
		}

		if (tok == NULL) {
			rc = xtc_notify_wait(slot.done, timeout_ns);
		} else {
			/* Slice the wait so the abort token is polled. */
			int64_t left = timeout_ns;
			const int64_t slice = 25LL * 1000 * 1000;   /* 25ms */
			for (;;) {
				int64_t w;
				if (xtc_abort_token_is_aborted(tok)) {
					rc = XTC_E_ABORTED;
					break;
				}
				w = (timeout_ns < 0) ? slice :
				    (left < slice ? left : slice);
				rc = xtc_notify_wait(slot.done, w);
				if (rc == XTC_OK) break;       /* reply arrived */
				if (timeout_ns >= 0) {
					left -= w;
					if (left <= 0) break;  /* timed out */
				}
			}
		}
		(void)__xtc_mtx_lock(&slot.lock);
		if (rc == XTC_OK) {
			rc = slot.rc;
			if (rc == XTC_OK) {
				*out_reply = slot.data;
				*out_size  = slot.size;
			} else {
				if (slot.data) __os_free(slot.data);
			}
		}
		(void)__xtc_mtx_unlock(&slot.lock);
		xtc_notify_destroy(slot.done);
		(void)pthread_mutex_destroy(&slot.lock);
		return rc;
	}
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

	if (call->slot != NULL) {
		struct __svr_reply_slot *slot = call->slot;
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
		(void)__xtc_mtx_lock(&slot->lock);
		slot->data = copy;
		slot->size = size;
		slot->rc   = XTC_OK;
		(void)xtc_notify_signal(slot->done);
		(void)__xtc_mtx_unlock(&slot->lock);
		rc = XTC_OK;
	} else if (!xtc_pid_is_none(call->reply_pid)) {
		/* Encode reply for in-proc caller: tag (4 bytes) + payload. */
		uint8_t *buf;
		size_t msg_size = 4 + size;
		buf = NULL;
		if (__os_malloc(msg_size, (void **)&buf) != XTC_OK)
			return XTC_E_NOMEM;
		memcpy(buf, &call->reply_tag, 4);
		if (size > 0) memcpy(buf + 4, reply, size);
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

/* In-proc receive helper: walk our own mailbox for a reply matching
 * `tag`, save other messages back to the queue. */
struct __tag_match { uint32_t tag; };
static int
__match_reply_tag(const void *data, size_t size, void *u)
{
	const struct __tag_match *m = u;
	if (size < 4) return 0;
	return memcmp(data, &m->tag, 4) == 0;
}

static int
__recv_reply_for_tag(uint32_t tag, void **out, size_t *out_size,
                     int64_t timeout_ns, xtc_abort_token_t *tok)
{
	struct __tag_match m = { tag };
	void  *msg = NULL;
	size_t size = 0;
	int    rc;

	if (tok == NULL) {
		rc = xtc_recv_match(__match_reply_tag, &m, &msg, &size,
		    timeout_ns);
	} else {
		/* Slice the receive so the abort token is polled between
		 * mailbox waits; XTC_E_ABORTED if it fires first. */
		int64_t left = timeout_ns;
		const int64_t slice = 25LL * 1000 * 1000;   /* 25ms */
		for (;;) {
			int64_t w;
			if (xtc_abort_token_is_aborted(tok)) return XTC_E_ABORTED;
			w = (timeout_ns < 0) ? slice : (left < slice ? left : slice);
			rc = xtc_recv_match(__match_reply_tag, &m, &msg, &size, w);
			if (rc != XTC_E_AGAIN) break;          /* matched or error */
			if (timeout_ns >= 0) {
				left -= w;
				if (left <= 0) break;          /* timed out */
			}
		}
	}
	if (rc != XTC_OK) return rc;
	if (size < 4) { __os_free(msg); return XTC_E_INVAL; }
	/* Strip the 4-byte tag prefix. */
	*out_size = size - 4;
	if (*out_size > 0) {
		void *copy = NULL;
		if (__os_malloc(*out_size, &copy) != XTC_OK) {
			__os_free(msg); return XTC_E_NOMEM;
		}
		memcpy(copy, (uint8_t *)msg + 4, *out_size);
		*out = copy;
	} else {
		*out = NULL;
	}
	__os_free(msg);
	return XTC_OK;
}
