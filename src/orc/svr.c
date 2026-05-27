/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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
 *	             encoded as little-endian uint64 — used by reply().
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
#include "xtc_svr.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static int __recv_reply_for_tag(uint32_t tag, void **out, size_t *out_size,
                                int64_t timeout_ns);

struct xtc_svr {
	xtc_loop_t           *loop;
	xtc_svr_callbacks_t   cb;
	void                 *state;
	xtc_pid_t             pid;
	xtc_notify_t         *stopped;
	_Atomic int           stop_requested;
	_Atomic int           alive;
};

/* Reply slot — owned by the caller of xtc_svr_call.  Lives on the
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
};

/* ----- entry ----------------------------------------------------- */

static void
__svr_entry(void *arg)
{
	struct xtc_svr *s = arg;
	s->pid = xtc_self();

	if (s->cb.init != NULL) {
		int rc = s->cb.init(s->state);
		if (rc != XTC_OK) goto out;
	}

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
			if (s->cb.handle_cast != NULL) {
				cont = s->cb.handle_cast(s->state,
				    (uint8_t *)msg + 1, size - 1);
			} else if (s->cb.handle_info != NULL) {
				cont = s->cb.handle_info(s->state, msg, size);
			}
		} else {
			if (s->cb.handle_info != NULL)
				cont = s->cb.handle_info(s->state, msg, size);
		}

		__os_free(msg);
		if (cont == XTC_SVR_STOP) break;
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
	if (s == NULL) return XTC_E_INVAL;
	(void)xtc_notify_wait(s->stopped, timeout_ns);
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

int
xtc_svr_call(xtc_pid_t target, const void *req, size_t req_size,
             void **out_reply, size_t *out_size, int64_t timeout_ns)
{
	if (req_size > 0 && req == NULL) return XTC_E_INVAL;
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

		buf = malloc(msg_size);
		if (buf == NULL) return XTC_E_NOMEM;
		buf[0] = 'C';
		buf[1] = 'p';
		memcpy(buf + 2, &self_pid, 8);
		memcpy(buf + 10, &tag, 4);
		if (req_size > 0) memcpy(buf + 14, req, req_size);

		rc = xtc_send(target, buf, msg_size);
		free(buf);
		if (rc != XTC_OK) return rc;

		return __recv_reply_for_tag(tag, out_reply, out_size, timeout_ns);
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
		buf = malloc(msg_size);
		if (buf == NULL) {
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
		free(buf);
		if (rc != XTC_OK) {
			xtc_notify_destroy(slot.done);
			(void)pthread_mutex_destroy(&slot.lock);
			return rc;
		}

		rc = xtc_notify_wait(slot.done, timeout_ns);
		(void)pthread_mutex_lock(&slot.lock);
		if (rc == XTC_OK) {
			rc = slot.rc;
			if (rc == XTC_OK) {
				*out_reply = slot.data;
				*out_size  = slot.size;
			} else {
				if (slot.data) free(slot.data);
			}
		}
		(void)pthread_mutex_unlock(&slot.lock);
		xtc_notify_destroy(slot.done);
		(void)pthread_mutex_destroy(&slot.lock);
		return rc;
	}
}

int
xtc_svr_cast(xtc_pid_t target, const void *msg, size_t size)
{
	uint8_t *buf;
	int rc;
	if (size > 0 && msg == NULL) return XTC_E_INVAL;
	buf = malloc(size + 1);
	if (buf == NULL) return XTC_E_NOMEM;
	buf[0] = 'X';
	if (size > 0) memcpy(buf + 1, msg, size);
	rc = xtc_send(target, buf, size + 1);
	free(buf);
	return rc;
}

int
xtc_svr_reply(xtc_svr_call_t *call, const void *reply, size_t size)
{
	if (call == NULL) return XTC_E_INVAL;

	if (call->slot != NULL) {
		struct __svr_reply_slot *slot = call->slot;
		void *copy = NULL;
		if (size > 0) {
			copy = malloc(size);
			if (copy == NULL) return XTC_E_NOMEM;
			memcpy(copy, reply, size);
		}
		(void)pthread_mutex_lock(&slot->lock);
		slot->data = copy;
		slot->size = size;
		slot->rc   = XTC_OK;
		(void)xtc_notify_signal(slot->done);
		(void)pthread_mutex_unlock(&slot->lock);
		return XTC_OK;
	}

	if (!xtc_pid_is_none(call->reply_pid)) {
		/* Encode reply for in-proc caller: tag (4 bytes) + payload. */
		uint8_t *buf;
		int rc;
		size_t msg_size = 4 + size;
		buf = malloc(msg_size);
		if (buf == NULL) return XTC_E_NOMEM;
		memcpy(buf, &call->reply_tag, 4);
		if (size > 0) memcpy(buf + 4, reply, size);
		rc = xtc_send(call->reply_pid, buf, msg_size);
		free(buf);
		return rc;
	}
	return XTC_E_INVAL;
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
                     int64_t timeout_ns)
{
	struct __tag_match m = { tag };
	void  *msg = NULL;
	size_t size = 0;
	int    rc = xtc_recv_match(__match_reply_tag, &m, &msg, &size, timeout_ns);
	if (rc != XTC_OK) return rc;
	if (size < 4) { __os_free(msg); return XTC_E_INVAL; }
	/* Strip the 4-byte tag prefix. */
	*out_size = size - 4;
	if (*out_size > 0) {
		void *copy = malloc(*out_size);
		if (copy == NULL) { __os_free(msg); return XTC_E_NOMEM; }
		memcpy(copy, (uint8_t *)msg + 4, *out_size);
		*out = copy;
	} else {
		*out = NULL;
	}
	__os_free(msg);
	return XTC_OK;
}
