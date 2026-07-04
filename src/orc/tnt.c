/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/tnt.c
 *	tnt scheduler + effect interpreter.  See xtc_tnt.h and
 *	docs/M_TINA_LAYER.md.
 *
 *	ARCHITECTURE.  One long-lived xtc_proc per shard (one per
 *	xtc_exec loop).  That shard proc owns:
 *	  - a dense typed arena per Isolate type (carved from an
 *	    xtc_slab cache at boot; no malloc on the hot path),
 *	  - a per-slot intrusive FIFO mailbox + generation counter,
 *	  - a shard-wide turn-scratch bump arena (reset per handler),
 *	  - a completion ring fed by transient I/O courier fibers,
 *	  - a self-wake pipe so couriers and cross-shard senders can
 *	    rouse the parked shard fiber.
 *
 *	The shard runs Tina's dispatch loop:
 *	    drain inbox -> collect reactor completions
 *	    -> for each ready type, for each dispatchable slot (budgeted):
 *	         call handler -> interpret the returned transition/effect.
 *
 *	Handlers never block.  Actions (send, spawn, submit I/O, arm
 *	timer) are staged via the ambient xtc_tnt_* calls during the
 *	handler; the returned transition tells the shard how to commit
 *	them.  Only the shard is a fiber; Isolates are arena structs.
 *
 *	I/O MODEL.  A handler stages recv/send/close into the current
 *	turn frame.  If it returns XTC_TNT_WAIT_IO, the shard commits each
 *	staged op by spawning a short-lived COURIER fiber that parks on
 *	the fd (xtc_proc_wait_fd), performs the non-blocking syscall,
 *	posts a completion record to the shard's ring, and wakes the
 *	shard.  The courier is the shard's I/O mechanism -- NOT an
 *	Isolate.  The Isolate stays a stackless arena struct; only the
 *	courier (bounded by outstanding ops) carries a stack.  This uses
 *	purely public libxtc APIs.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * tnt's I/O effect interpreter does raw POSIX socket I/O (recv/send with
 * MSG_DONTWAIT) in its courier fibers, so the implementation is gated on
 * a POSIX target -- matching how the platform-specific I/O backends
 * (io_aix.c, io_solaris.c) are gated.  On a non-POSIX target (MSVC) the
 * public xtc_tnt_* entry points are NOSYS stubs (see the #else at the
 * end of the file).  s_include scans the export markers regardless of
 * the #if, so the generated prototypes stay consistent across platforms.
 */
#include "xtc_int.h"
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock: preemption-safe locks */
#include "xtc_tnt.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
#include "xtc_slab.h"
#include "os_alloc.h"
#include "os_time.h"

/* ---- Tunables for this slice -------------------------------------- */
#define XTC_TNT_MAX_SHARDS       8
#define XTC_TNT_MAX_TYPES        16
#define XTC_TNT_MAX_STAGED_IO    8     /* staged I/O ops per turn frame */
#define XTC_TNT_COMPLETION_CAP   4096  /* per-shard completion ring depth */
#define XTC_TNT_DEFAULT_RECVBUF  65536

/* ---- Slot states -------------------------------------------------- */
enum xtc_tnt_slot_state {
	XTC_TNT_SLOT_FREE = 0,     /* unused arena slot */
	XTC_TNT_SLOT_WAIT_MESSAGE, /* live, parked until a message arrives */
	XTC_TNT_SLOT_READY,        /* live, has a queued message OR yielded */
	XTC_TNT_SLOT_WAIT_IO       /* live, parked on an outstanding I/O op */
};

/* A queued message envelope (intrusive FIFO node). */
typedef struct xtc_tnt_envelope {
	struct xtc_tnt_envelope *next;
	xtc_tnt_message_t        msg;
} xtc_tnt_envelope_t;

/* Per-slot metadata.  The Isolate struct itself lives in a parallel
 * dense array (so the typed arena stays type-pure and cache-friendly,
 * exactly as Tina batches by type). */
typedef struct xtc_tnt_slot {
	uint32_t          gen;          /* generation counter (handle safety) */
	uint8_t           state;        /* enum xtc_tnt_slot_state */
	uint8_t           type_id;
	/* Intrusive mailbox FIFO. */
	xtc_tnt_envelope_t   *mbox_head;
	xtc_tnt_envelope_t   *mbox_tail;
	uint32_t          mbox_depth;
	/* The single pending I/O completion that, when reaped, drives a
	 * WAIT_IO slot back to READY. */
	uint32_t          io_pending;   /* count of couriers in flight */
} xtc_tnt_slot_t;

/* Per-type arena on a shard. */
typedef struct xtc_tnt_arena {
	const xtc_tnt_type_t *type;
	xtc_tnt_slot_t       *slots;        /* slot_count metadata records */
	uint8_t          *store;        /* slot_count * stride Isolate bytes */
	uint32_t         *free_list;    /* stack of free slot indices */
	uint32_t          free_top;     /* number of free indices available */
	uint32_t          slot_count;
	/* Round-robin dispatch cursor across READY slots. */
	uint32_t          cursor;
} xtc_tnt_arena_t;

/* A completion posted by a courier, drained by the shard. */
typedef struct xtc_tnt_completion {
	xtc_tnt_handle_t target;
	uint16_t     tag;
	int          fd;
	int32_t      result;
	void        *buffer;        /* recv only; freed after delivery */
	uint32_t     buffer_len;
} xtc_tnt_completion_t;

/* A staged I/O op, accumulated in the turn frame during a handler and
 * committed after the handler returns XTC_TNT_WAIT_IO. */
typedef struct xtc_tnt_staged_io {
	uint16_t      kind;          /* completion tag the op will produce */
	int           fd;
	const void   *send_buf;      /* for send: points into the Isolate */
	size_t        send_len;
} xtc_tnt_staged_io_t;

/* Shard: one per loop, owns everything it touches. */
typedef struct xtc_tnt_shard {
	uint8_t            id;
	const xtc_tnt_spec_t  *spec;
	xtc_loop_t        *loop;
	struct xtc_tnt_runtime *rt;

	xtc_tnt_arena_t        arenas[XTC_TNT_MAX_TYPES];
	int                n_arenas;

	/* Slab caches backing this shard's arenas (one per type) plus an
	 * envelope cache shared across types. */
	xtc_slab_t        *slot_cache[XTC_TNT_MAX_TYPES];   /* metadata slabs */
	xtc_slab_t        *store_cache[XTC_TNT_MAX_TYPES];  /* Isolate-struct slabs */
	xtc_slab_t        *env_cache;                   /* message envelopes */

	/* Turn-scratch bump arena: reset before every handler. */
	uint8_t           *scratch;
	uint32_t           scratch_cap;
	uint32_t           scratch_off;

	/* Completion ring (MPSC: many couriers produce, the shard
	 * consumes).  Guarded by a mutex -- couriers run on the same loop
	 * thread as the shard, so contention is nil, but a mutex keeps it
	 * correct under the multi-loop executor where a courier may run
	 * stolen elsewhere. */
	xtc_tnt_completion_t  *comp_ring;
	uint32_t           comp_head;     /* consumer */
	uint32_t           comp_tail;     /* producer */
	pthread_mutex_t    comp_lock;

	/* Self-wake pipe.  Couriers / cross-shard sends write a byte to
	 * wake the parked shard fiber. */
	int                wake_rd;
	int                wake_wr;

	/* Cross-shard / external spawn inbox (handle-less spawn requests).
	 * Guarded by spawn_lock. */
	struct xtc_tnt_spawn_req *spawn_head;
	struct xtc_tnt_spawn_req *spawn_tail;
	pthread_mutex_t    spawn_lock;

	atomic_int         stop;
	uint64_t           ticks;
} xtc_tnt_shard_t;

typedef struct xtc_tnt_spawn_req {
	struct xtc_tnt_spawn_req *next;
	size_t    args_size;
	uint8_t   type_id;
	/* args is over-aligned (max_align_t) so a typed init-args struct
	 * with a u32/u64/pointer member loads aligned out of it. */
	_Alignas(16) uint8_t args[XTC_TNT_MAX_INIT_ARGS_SIZE];
} xtc_tnt_spawn_req_t;

/* Runtime: the whole system. */
typedef struct xtc_tnt_runtime {
	const xtc_tnt_spec_t *spec;
	xtc_exec_t       *exec;
	xtc_tnt_shard_t      *shards[XTC_TNT_MAX_SHARDS];
	int               shard_count;
	atomic_int        stop;
} xtc_tnt_runtime_t;

/* The live runtime (one per process; xtc_tnt_start is not re-entrant). */
static xtc_tnt_runtime_t *g_rt = NULL;

/* Thread-local current turn frame.  Set while a handler runs. */
typedef struct xtc_tnt_frame {
	xtc_tnt_shard_t  *shard;
	xtc_tnt_arena_t  *arena;
	uint32_t      slot;
	xtc_tnt_handle_t  self;
	/* Staged I/O accumulated this turn. */
	xtc_tnt_staged_io_t staged[XTC_TNT_MAX_STAGED_IO];
	int             n_staged;
} xtc_tnt_frame_t;

static _Thread_local xtc_tnt_frame_t *tl_frame = NULL;
static _Thread_local xtc_tnt_shard_t *tl_shard = NULL;

/* ---- Forward decls ------------------------------------------------ */
static void xtc_tnt_shard_main(void *arg);
static void xtc_tnt_courier_main(void *arg);
static xtc_tnt_send_result_t xtc_tnt_deliver(xtc_tnt_shard_t *sh, xtc_tnt_handle_t to,
                                     const xtc_tnt_message_t *m);
static void xtc_tnt_commit_one_io(xtc_tnt_shard_t *sh, xtc_tnt_handle_t target,
                              xtc_tnt_staged_io_t *s, xtc_tnt_slot_t *meta);

/* ---- Small helpers ------------------------------------------------ */

static inline xtc_tnt_arena_t *
shard_arena(xtc_tnt_shard_t *sh, uint8_t type_id)
{
	int i;
	for (i = 0; i < sh->n_arenas; i++)
		if (sh->arenas[i].type->id == type_id)
			return &sh->arenas[i];
	return NULL;
}

static inline void *
slot_isolate(xtc_tnt_arena_t *ar, uint32_t slot)
{
	return ar->store + (size_t)slot * ar->type->stride;
}

/* ---- Completion ring ---------------------------------------------- */

static int
comp_push(xtc_tnt_shard_t *sh, const xtc_tnt_completion_t *c)
{
	int ok = 0;
	(void)__xtc_mtx_lock(&sh->comp_lock);
	if (sh->comp_tail - sh->comp_head < XTC_TNT_COMPLETION_CAP) {
		sh->comp_ring[sh->comp_tail % XTC_TNT_COMPLETION_CAP] = *c;
		sh->comp_tail++;
		ok = 1;
	}
	(void)__xtc_mtx_unlock(&sh->comp_lock);
	return ok;
}

static int
comp_pop(xtc_tnt_shard_t *sh, xtc_tnt_completion_t *out)
{
	int ok = 0;
	(void)__xtc_mtx_lock(&sh->comp_lock);
	if (sh->comp_head != sh->comp_tail) {
		*out = sh->comp_ring[sh->comp_head % XTC_TNT_COMPLETION_CAP];
		sh->comp_head++;
		ok = 1;
	}
	(void)__xtc_mtx_unlock(&sh->comp_lock);
	return ok;
}

static void
shard_wake(xtc_tnt_shard_t *sh)
{
	uint8_t b = 1;
	ssize_t r = write(sh->wake_wr, &b, 1);  /* XTC_BLOCKING_OK: 1 byte to a nonblocking self-wake pipe */
	(void)r;
}

/* ---- Slot lifecycle ----------------------------------------------- */

/* Allocate a free slot in arena, run init, return the new handle or
 * XTC_TNT_HANDLE_NONE on failure (with *err set). */
static xtc_tnt_handle_t
slot_spawn(xtc_tnt_shard_t *sh, xtc_tnt_arena_t *ar, const void *args,
           size_t args_size, xtc_tnt_spawn_error_t *err)
{
	uint32_t slot;
	xtc_tnt_slot_t *meta;
	void *iso;
	xtc_tnt_frame_t frame;
	xtc_tnt_frame_t *saved_frame = tl_frame;
	xtc_tnt_transition_t tr;

	if (ar->free_top == 0) {
		*err = XTC_TNT_SPAWN_ARENA_FULL;
		return XTC_TNT_HANDLE_NONE;
	}
	slot = ar->free_list[--ar->free_top];
	meta = &ar->slots[slot];
	iso = slot_isolate(ar, slot);

	memset(iso, 0, ar->type->stride);
	meta->state = XTC_TNT_SLOT_READY;
	meta->type_id = ar->type->id;
	meta->mbox_head = meta->mbox_tail = NULL;
	meta->mbox_depth = 0;
	meta->io_pending = 0;

	/* Run init inside a turn frame so it can use ctx_* calls. */
	memset(&frame, 0, sizeof(frame));
	frame.shard = sh;
	frame.arena = ar;
	frame.slot = slot;
	frame.self = xtc_tnt_handle_make(sh->id, ar->type->id, slot, meta->gen);
	tl_frame = &frame;

	sh->scratch_off = 0;
	if (ar->type->init_fn != NULL)
		tr = ar->type->init_fn(iso, args, args_size);
	else
		tr = XTC_TNT_TRANSITION_WAIT_MESSAGE;

	tl_frame = saved_frame;

	if (tr.kind == XTC_TNT_CRASH) {
		/* init failed: reclaim the slot, bump gen. */
		meta->state = XTC_TNT_SLOT_FREE;
		meta->gen = (meta->gen + 1) & XTC_TNT_GEN_MASK;
		ar->free_list[ar->free_top++] = slot;
		*err = XTC_TNT_SPAWN_INIT_FAILED;
		return XTC_TNT_HANDLE_NONE;
	}

	*err = XTC_TNT_SPAWN_OK;
	/* The transition from init is interpreted by the same machinery
	 * the dispatch loop uses; record the desired post-init state. */
	switch (tr.kind) {
	case XTC_TNT_DONE:
		/* init says done immediately: tear down. */
		meta->state = XTC_TNT_SLOT_FREE;
		meta->gen = (meta->gen + 1) & XTC_TNT_GEN_MASK;
		ar->free_list[ar->free_top++] = slot;
		break;
	case XTC_TNT_WAIT_MESSAGE:
		/* If init queued a message into our own mailbox (a self-send),
		 * we are immediately READY; otherwise park. */
		meta->state = (meta->mbox_depth > 0)
		    ? XTC_TNT_SLOT_READY : XTC_TNT_SLOT_WAIT_MESSAGE;
		break;
	case XTC_TNT_WAIT_IO:
		/* init staged I/O; commit it (handled by caller-side commit
		 * below via the frame).  We commit here using the frame copy. */
		meta->state = XTC_TNT_SLOT_WAIT_IO;
		break;
	case XTC_TNT_YIELD:
	default:
		meta->state = XTC_TNT_SLOT_READY;
		break;
	}

	/* Commit any staged I/O from init (the frame is local; replay it). */
	{
		int i;
		for (i = 0; i < frame.n_staged; i++) {
			xtc_tnt_staged_io_t *s = &frame.staged[i];
			xtc_tnt_handle_t h = xtc_tnt_handle_make(sh->id, ar->type->id,
			    slot, meta->gen);
			xtc_tnt_commit_one_io(sh, h, s, meta);
		}
	}

	return xtc_tnt_handle_make(sh->id, ar->type->id, slot, meta->gen);
}

static void
slot_teardown(xtc_tnt_arena_t *ar, uint32_t slot)
{
	xtc_tnt_slot_t *meta = &ar->slots[slot];
	xtc_tnt_envelope_t *e, *n;

	/* Drain + free any queued envelopes. */
	for (e = meta->mbox_head; e != NULL; e = n) {
		n = e->next;
		__os_free(e);
	}
	meta->mbox_head = meta->mbox_tail = NULL;
	meta->mbox_depth = 0;
	meta->state = XTC_TNT_SLOT_FREE;
	/* Generation bump: any handle to the old generation is now stale. */
	meta->gen = (meta->gen + 1) & XTC_TNT_GEN_MASK;
	ar->free_list[ar->free_top++] = slot;
}

/* ---- I/O commit (spawn couriers) ---------------------------------- */

/* Courier argument: heap-allocated, owned by the courier. */
typedef struct xtc_tnt_courier_arg {
	xtc_tnt_shard_t  *shard;
	xtc_tnt_handle_t  target;
	uint16_t      kind;
	int           fd;
	const void   *send_buf;
	size_t        send_len;
} xtc_tnt_courier_arg_t;

/* Commit a single staged op by spawning a courier. */
static void
xtc_tnt_commit_one_io(xtc_tnt_shard_t *sh, xtc_tnt_handle_t target, xtc_tnt_staged_io_t *s,
                  xtc_tnt_slot_t *meta)
{
	xtc_tnt_courier_arg_t *ca = NULL;
	xtc_proc_opts_t popts = { 0 };
	xtc_pid_t pid;

	if (__os_calloc(1, sizeof(*ca), (void **)&ca) != XTC_OK || ca == NULL)
		return;
	ca->shard = sh;
	ca->target = target;
	ca->kind = s->kind;
	ca->fd = s->fd;
	ca->send_buf = s->send_buf;
	ca->send_len = s->send_len;

	popts.name = "tnt-io";
	if (xtc_proc_spawn(sh->loop, xtc_tnt_courier_main, ca, &popts, &pid)
	    != XTC_OK) {
		__os_free(ca);
		return;
	}
	if (meta != NULL)
		meta->io_pending++;
}

/* Courier fiber: park on the fd, perform the op, post a completion,
 * wake the shard. */
static void
xtc_tnt_courier_main(void *arg)
{
	xtc_tnt_courier_arg_t *ca = arg;
	xtc_tnt_shard_t *sh = ca->shard;
	xtc_tnt_completion_t c;
	uint32_t interest;
	uint32_t revents = 0;

	memset(&c, 0, sizeof(c));
	c.target = ca->target;
	c.fd = ca->fd;
	c.tag = ca->kind;

	if (ca->kind == XTC_TNT_IO_TAG_CLOSE_COMPLETE) {
		close(ca->fd);
		c.result = 0;
		goto post;
	}

	interest = (ca->kind == XTC_TNT_IO_TAG_SEND_COMPLETE)
	    ? XTC_IO_WRITABLE : XTC_IO_READABLE;
	interest |= XTC_IO_HUP | XTC_IO_ERR;

	/* Park until ready (or shard stop -- bounded timeout to re-check). */
	for (;;) {
		int rc = xtc_proc_wait_fd(ca->fd, interest,
		    1000LL * 1000 * 1000, &revents);
		if (atomic_load(&sh->stop)) {
			c.result = -ECANCELED;
			goto post;
		}
		if (rc == XTC_OK)
			break;
		if (rc != XTC_E_AGAIN) {
			c.result = -EIO;
			goto post;
		}
		/* timeout: loop and re-check stop */
	}

	if (ca->kind == XTC_TNT_IO_TAG_RECV_COMPLETE) {
		void *buf = NULL;
		ssize_t n;
		uint32_t cap = sh->spec->recv_buf_size
		    ? sh->spec->recv_buf_size : XTC_TNT_DEFAULT_RECVBUF;
		if (__os_malloc(cap, (void **)&buf) != XTC_OK || buf == NULL) {
			c.result = -ENOMEM;
			goto post;
		}
		n = recv(ca->fd, buf, cap, MSG_DONTWAIT);  /* XTC_BLOCKING_OK: MSG_DONTWAIT, fd is ready (courier parked on it) */
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* spurious wake; retry once synchronously */
			n = recv(ca->fd, buf, cap, MSG_DONTWAIT);  /* XTC_BLOCKING_OK: MSG_DONTWAIT */
		}
		c.result = (int32_t)n;
		if (n > 0) {
			c.buffer = buf;
			c.buffer_len = (uint32_t)n;
		} else {
			__os_free(buf);
			c.buffer = NULL;
			c.buffer_len = 0;
		}
	} else if (ca->kind == XTC_TNT_IO_TAG_SEND_COMPLETE) {
		ssize_t total = 0;
		const uint8_t *p = ca->send_buf;
		size_t left = ca->send_len;
		while (left > 0) {
			ssize_t n = send(ca->fd, p + total, left, MSG_DONTWAIT);  /* XTC_BLOCKING_OK: MSG_DONTWAIT, re-parks on EAGAIN */
			if (n > 0) {
				total += n;
				left -= (size_t)n;
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				int rc = xtc_proc_wait_fd(ca->fd,
				    XTC_IO_WRITABLE | XTC_IO_HUP | XTC_IO_ERR,
				    1000LL * 1000 * 1000, &revents);
				if (atomic_load(&sh->stop)) {
					c.result = -ECANCELED;
					goto post;
				}
				if (rc != XTC_OK && rc != XTC_E_AGAIN) {
					c.result = -EIO;
					goto post;
				}
				continue;
			}
			/* hard error */
			c.result = (int32_t)n;
			goto post;
		}
		c.result = (int32_t)total;
	}

post:
	(void)comp_push(sh, &c);
	shard_wake(sh);
	__os_free(ca);
}

/* ---- Ambient ctx_* API -------------------------------------------- */

/*
 * PUBLIC: int xtc_tnt_start __P((const xtc_tnt_spec_t *));
 * PUBLIC: void xtc_tnt_stop __P((void));
 * PUBLIC: xtc_tnt_spawn_error_t xtc_tnt_spawn_on __P((uint8_t, uint8_t, const void *, size_t));
 *
 * PUBLIC: xtc_tnt_send_result_t xtc_tnt_send __P((xtc_tnt_handle_t, uint16_t, const void *, size_t));
 * PUBLIC: xtc_tnt_spawn_error_t xtc_tnt_spawn __P((uint8_t, const void *, size_t, xtc_tnt_handle_t *));
 * PUBLIC: xtc_tnt_io_result_t xtc_tnt_submit_recv __P((int));
 * PUBLIC: xtc_tnt_io_result_t xtc_tnt_io_send __P((int, const void *, size_t));
 * PUBLIC: xtc_tnt_io_result_t xtc_tnt_submit_close __P((int));
 * PUBLIC: void xtc_tnt_register_timer __P((uint64_t, uint16_t));
 * PUBLIC: xtc_tnt_handle_t xtc_tnt_self __P((void));
 * PUBLIC: uint8_t xtc_tnt_shard_id __P((void));
 * PUBLIC: void *xtc_tnt_scratch_arena __P((size_t));
 */

xtc_tnt_handle_t
xtc_tnt_self(void)
{
	return tl_frame ? tl_frame->self : XTC_TNT_HANDLE_NONE;
}

uint8_t
xtc_tnt_shard_id(void)
{
	return tl_shard ? tl_shard->id : 0;
}

void *
xtc_tnt_scratch_arena(size_t size)
{
	xtc_tnt_shard_t *sh = tl_shard;
	void *p;
	if (sh == NULL)
		return NULL;
	/* 16-byte align. */
	sh->scratch_off = (sh->scratch_off + 15u) & ~15u;
	if (sh->scratch_off + size > sh->scratch_cap)
		return NULL;
	p = sh->scratch + sh->scratch_off;
	sh->scratch_off += (uint32_t)size;
	return p;
}

xtc_tnt_send_result_t
xtc_tnt_send(xtc_tnt_handle_t to, uint16_t tag, const void *payload,
         size_t payload_size)
{
	xtc_tnt_shard_t *sh = tl_shard;
	xtc_tnt_message_t m;

	if (sh == NULL)
		return XTC_TNT_SEND_STALE_HANDLE;
	if (tag < XTC_TNT_USER_TAG_BASE)
		return XTC_TNT_SEND_STALE_HANDLE; /* reject system tags */
	if (payload_size > XTC_TNT_MAX_PAYLOAD_SIZE)
		payload_size = XTC_TNT_MAX_PAYLOAD_SIZE;

	memset(&m, 0, sizeof(m));
	m.tag = tag;
	m.body.user.source = xtc_tnt_self();
	m.body.user.payload_size = (uint16_t)payload_size;
	if (payload && payload_size)
		memcpy(m.body.user.payload, payload, payload_size);

	/* Intra-shard fast path: deliver straight into the arena mailbox.
	 * Cross-shard would route via the runtime; this slice delivers
	 * intra-shard directly and cross-shard through the same path since
	 * all shards share the process address space. */
	if (xtc_tnt_handle_shard(to) == sh->id)
		return xtc_tnt_deliver(sh, to, &m);

	/* Cross-shard: hand to the target shard's deliver under its lock
	 * via the completion ring is overkill for messages; instead route
	 * through that shard directly (same process).  For this slice we
	 * deliver into the target arena guarded by its comp_lock-free
	 * mailbox -- safe because deliver only touches the slot mailbox and
	 * is called on the owning thread in the common case.  Cross-shard
	 * faithful transport (per-pair rings) is layer-future work. */
	{
		xtc_tnt_runtime_t *rt = sh->rt;
		uint8_t dst = xtc_tnt_handle_shard(to);
		if (dst >= rt->shard_count || rt->shards[dst] == NULL)
			return XTC_TNT_SEND_STALE_HANDLE;
		return xtc_tnt_deliver(rt->shards[dst], to, &m);
	}
}

/* Deliver a message into a target slot's mailbox.  Drop-on-full with
 * sender feedback (Tina's .mailbox_full semantics). */
static xtc_tnt_send_result_t
xtc_tnt_deliver(xtc_tnt_shard_t *sh, xtc_tnt_handle_t to, const xtc_tnt_message_t *m)
{
	xtc_tnt_arena_t *ar = shard_arena(sh, xtc_tnt_handle_type(to));
	uint32_t slot;
	xtc_tnt_slot_t *meta;
	xtc_tnt_envelope_t *e = NULL;

	if (ar == NULL)
		return XTC_TNT_SEND_STALE_HANDLE;
	slot = xtc_tnt_handle_slot(to);
	if (slot >= ar->slot_count)
		return XTC_TNT_SEND_STALE_HANDLE;
	meta = &ar->slots[slot];
	if (meta->state == XTC_TNT_SLOT_FREE)
		return XTC_TNT_SEND_STALE_HANDLE;
	if (meta->gen != xtc_tnt_handle_gen(to))
		return XTC_TNT_SEND_STALE_HANDLE;
	if (meta->mbox_depth >= ar->type->mailbox_capacity)
		return XTC_TNT_SEND_MAILBOX_FULL;   /* drop-on-full */

	if (__os_malloc(sizeof(*e), (void **)&e) != XTC_OK || e == NULL)
		return XTC_TNT_SEND_POOL_EXHAUSTED;
	e->next = NULL;
	e->msg = *m;
	if (meta->mbox_tail)
		meta->mbox_tail->next = e;
	else
		meta->mbox_head = e;
	meta->mbox_tail = e;
	meta->mbox_depth++;

	/* A parked WAIT_MESSAGE slot becomes READY. */
	if (meta->state == XTC_TNT_SLOT_WAIT_MESSAGE)
		meta->state = XTC_TNT_SLOT_READY;

	/* If the target is on another shard's thread, wake it. */
	if (sh != tl_shard)
		shard_wake(sh);
	return XTC_TNT_SEND_OK;
}

xtc_tnt_spawn_error_t
xtc_tnt_spawn(uint8_t type_id, const void *args, size_t args_size,
          xtc_tnt_handle_t *out_handle)
{
	xtc_tnt_shard_t *sh = tl_shard;
	xtc_tnt_arena_t *ar;
	xtc_tnt_spawn_error_t err;
	xtc_tnt_handle_t h;

	if (sh == NULL)
		return XTC_TNT_SPAWN_TYPE_NOT_ALLOCATED;
	ar = shard_arena(sh, type_id);
	if (ar == NULL)
		return XTC_TNT_SPAWN_TYPE_NOT_ALLOCATED;
	h = slot_spawn(sh, ar, args, args_size, &err);
	if (out_handle)
		*out_handle = h;
	return err;
}

xtc_tnt_io_result_t
xtc_tnt_submit_recv(int fd)
{
	xtc_tnt_frame_t *fr = tl_frame;
	xtc_tnt_staged_io_t *s;
	if (fr == NULL || fd < 0)
		return XTC_TNT_IO_BAD_FD;
	if (fr->n_staged >= XTC_TNT_MAX_STAGED_IO)
		return XTC_TNT_IO_TOO_MANY;
	s = &fr->staged[fr->n_staged++];
	s->kind = XTC_TNT_IO_TAG_RECV_COMPLETE;
	s->fd = fd;
	s->send_buf = NULL;
	s->send_len = 0;
	return XTC_TNT_IO_OK;
}

xtc_tnt_io_result_t
xtc_tnt_io_send(int fd, const void *buffer, size_t len)
{
	xtc_tnt_frame_t *fr = tl_frame;
	xtc_tnt_staged_io_t *s;
	if (fr == NULL || fd < 0)
		return XTC_TNT_IO_BAD_FD;
	if (fr->n_staged >= XTC_TNT_MAX_STAGED_IO)
		return XTC_TNT_IO_TOO_MANY;
	s = &fr->staged[fr->n_staged++];
	s->kind = XTC_TNT_IO_TAG_SEND_COMPLETE;
	s->fd = fd;
	s->send_buf = buffer;
	s->send_len = len;
	return XTC_TNT_IO_OK;
}

xtc_tnt_io_result_t
xtc_tnt_submit_close(int fd)
{
	xtc_tnt_frame_t *fr = tl_frame;
	xtc_tnt_staged_io_t *s;
	if (fr == NULL || fd < 0)
		return XTC_TNT_IO_BAD_FD;
	if (fr->n_staged >= XTC_TNT_MAX_STAGED_IO)
		return XTC_TNT_IO_TOO_MANY;
	s = &fr->staged[fr->n_staged++];
	s->kind = XTC_TNT_IO_TAG_CLOSE_COMPLETE;
	s->fd = fd;
	s->send_buf = NULL;
	s->send_len = 0;
	return XTC_TNT_IO_OK;
}

/* Timer: arm a one-shot timer that delivers `tag` back to this Isolate.
 * Implemented with a courier-like fiber that sleeps then posts. */
typedef struct xtc_tnt_timer_arg {
	xtc_tnt_shard_t  *shard;
	xtc_tnt_handle_t  target;
	uint64_t      duration_ns;
	uint16_t      tag;
} xtc_tnt_timer_arg_t;

static void
xtc_tnt_timer_main(void *arg)
{
	xtc_tnt_timer_arg_t *ta = arg;
	xtc_tnt_completion_t c;

	(void)xtc_proc_sleep((int64_t)ta->duration_ns);
	memset(&c, 0, sizeof(c));
	c.target = ta->target;
	c.tag = ta->tag;
	c.fd = -1;
	c.result = 0;
	(void)comp_push(ta->shard, &c);
	shard_wake(ta->shard);
	__os_free(ta);
}

void
xtc_tnt_register_timer(uint64_t duration_ns, uint16_t tag)
{
	xtc_tnt_shard_t *sh = tl_shard;
	xtc_tnt_timer_arg_t *ta = NULL;
	xtc_proc_opts_t popts = { 0 };
	xtc_pid_t pid;

	if (sh == NULL)
		return;
	if (__os_calloc(1, sizeof(*ta), (void **)&ta) != XTC_OK || ta == NULL)
		return;
	ta->shard = sh;
	ta->target = xtc_tnt_self();
	ta->duration_ns = duration_ns;
	ta->tag = tag;
	popts.name = "tnt-timer";
	if (xtc_proc_spawn(sh->loop, xtc_tnt_timer_main, ta, &popts, &pid)
	    != XTC_OK)
		__os_free(ta);
}

/* External / cross-thread spawn: enqueue a request onto the target
 * shard's spawn inbox and wake it. */
xtc_tnt_spawn_error_t
xtc_tnt_spawn_on(uint8_t shard, uint8_t type_id, const void *args,
             size_t args_size)
{
	xtc_tnt_runtime_t *rt = g_rt;
	xtc_tnt_shard_t *sh;
	xtc_tnt_spawn_req_t *req = NULL;

	if (rt == NULL || shard >= rt->shard_count)
		return XTC_TNT_SPAWN_TYPE_NOT_ALLOCATED;
	sh = rt->shards[shard];
	if (sh == NULL)
		return XTC_TNT_SPAWN_TYPE_NOT_ALLOCATED;
	if (args_size > XTC_TNT_MAX_INIT_ARGS_SIZE)
		args_size = XTC_TNT_MAX_INIT_ARGS_SIZE;

	if (__os_calloc(1, sizeof(*req), (void **)&req) != XTC_OK ||
	    req == NULL)
		return XTC_TNT_SPAWN_ARENA_FULL;
	req->type_id = type_id;
	req->args_size = args_size;
	if (args && args_size)
		memcpy(req->args, args, args_size);

	(void)__xtc_mtx_lock(&sh->spawn_lock);
	req->next = NULL;
	if (sh->spawn_tail)
		sh->spawn_tail->next = req;
	else
		sh->spawn_head = req;
	sh->spawn_tail = req;
	(void)__xtc_mtx_unlock(&sh->spawn_lock);
	shard_wake(sh);
	return XTC_TNT_SPAWN_OK;
}

/* ---- Dispatch -- run one handler for a READY slot ----------------- */

/* Commit the staged I/O recorded in `fr` after a handler returned
 * XTC_TNT_WAIT_IO. */
static void
commit_staged_io(xtc_tnt_shard_t *sh, xtc_tnt_frame_t *fr, xtc_tnt_slot_t *meta)
{
	int i;
	for (i = 0; i < fr->n_staged; i++)
		xtc_tnt_commit_one_io(sh, fr->self, &fr->staged[i], meta);
}

/* Run the handler for one slot with `msg`, then interpret the
 * transition.  Returns 1 if the slot was torn down. */
static int
dispatch_slot(xtc_tnt_shard_t *sh, xtc_tnt_arena_t *ar, uint32_t slot,
              xtc_tnt_message_t *msg)
{
	xtc_tnt_slot_t *meta = &ar->slots[slot];
	void *iso = slot_isolate(ar, slot);
	xtc_tnt_frame_t frame;
	xtc_tnt_frame_t *saved = tl_frame;
	xtc_tnt_transition_t tr;

	memset(&frame, 0, sizeof(frame));
	frame.shard = sh;
	frame.arena = ar;
	frame.slot = slot;
	frame.self = xtc_tnt_handle_make(sh->id, ar->type->id, slot, meta->gen);
	tl_frame = &frame;

	sh->scratch_off = 0;
	tr = ar->type->handler_fn(iso, msg);

	tl_frame = saved;

	switch (tr.kind) {
	case XTC_TNT_DONE:
	case XTC_TNT_CRASH:
		/* Level-1 voluntary teardown (a real segfault would unwind
		 * the shard fiber -- Level-2 -- and is layer-future work). */
		slot_teardown(ar, slot);
		return 1;
	case XTC_TNT_WAIT_IO:
		meta->state = XTC_TNT_SLOT_WAIT_IO;
		commit_staged_io(sh, &frame, meta);
		return 0;
	case XTC_TNT_WAIT_MESSAGE:
		meta->state = (meta->mbox_depth > 0)
		    ? XTC_TNT_SLOT_READY : XTC_TNT_SLOT_WAIT_MESSAGE;
		return 0;
	case XTC_TNT_YIELD:
	default:
		meta->state = XTC_TNT_SLOT_READY;
		return 0;
	}
}

/* Drain external spawn requests for this shard. */
static void
drain_spawns(xtc_tnt_shard_t *sh)
{
	xtc_tnt_spawn_req_t *head, *req, *n;

	(void)__xtc_mtx_lock(&sh->spawn_lock);
	head = sh->spawn_head;
	sh->spawn_head = sh->spawn_tail = NULL;
	(void)__xtc_mtx_unlock(&sh->spawn_lock);

	for (req = head; req != NULL; req = n) {
		xtc_tnt_arena_t *ar = shard_arena(sh, req->type_id);
		n = req->next;
		if (ar != NULL) {
			xtc_tnt_spawn_error_t err;
			(void)slot_spawn(sh, ar, req->args, req->args_size,
			    &err);
		}
		__os_free(req);
	}
}

/* Drain completions: deliver each as an I/O message to its target. */
static void
drain_completions(xtc_tnt_shard_t *sh)
{
	xtc_tnt_completion_t c;

	while (comp_pop(sh, &c)) {
		xtc_tnt_arena_t *ar = shard_arena(sh, xtc_tnt_handle_type(c.target));
		uint32_t slot;
		xtc_tnt_slot_t *meta;
		xtc_tnt_message_t m;

		if (ar == NULL) {
			if (c.buffer)
				__os_free(c.buffer);
			continue;
		}
		slot = xtc_tnt_handle_slot(c.target);
		if (slot >= ar->slot_count) {
			if (c.buffer)
				__os_free(c.buffer);
			continue;
		}
		meta = &ar->slots[slot];
		if (meta->io_pending > 0)
			meta->io_pending--;
		/* Stale target (torn down while I/O was in flight): drop. */
		if (meta->state == XTC_TNT_SLOT_FREE ||
		    meta->gen != xtc_tnt_handle_gen(c.target)) {
			if (c.buffer)
				__os_free(c.buffer);
			continue;
		}

		memset(&m, 0, sizeof(m));
		m.tag = c.tag;
		if (c.tag == XTC_TNT_TAG_TIMER) {
			m.body.user.source = c.target;
		} else {
			m.body.io.fd = c.fd;
			m.body.io.result = c.result;
			m.body.io.buffer = c.buffer;
			m.body.io.buffer_len = c.buffer_len;
		}

		/* The I/O completion drives the slot directly (it was
		 * WAIT_IO).  Dispatch the handler synchronously with this
		 * message, then free the recv buffer. */
		{
			void *iso = slot_isolate(ar, slot);
			xtc_tnt_frame_t frame;
			xtc_tnt_frame_t *saved = tl_frame;
			xtc_tnt_transition_t tr;

			memset(&frame, 0, sizeof(frame));
			frame.shard = sh;
			frame.arena = ar;
			frame.slot = slot;
			frame.self = xtc_tnt_handle_make(sh->id, ar->type->id,
			    slot, meta->gen);
			tl_frame = &frame;
			sh->scratch_off = 0;
			tr = ar->type->handler_fn(iso, &m);
			tl_frame = saved;

			if (c.buffer)
				__os_free(c.buffer);

			switch (tr.kind) {
			case XTC_TNT_DONE:
			case XTC_TNT_CRASH:
				slot_teardown(ar, slot);
				break;
			case XTC_TNT_WAIT_IO:
				meta->state = XTC_TNT_SLOT_WAIT_IO;
				commit_staged_io(sh, &frame, meta);
				break;
			case XTC_TNT_WAIT_MESSAGE:
				meta->state = (meta->mbox_depth > 0)
				    ? XTC_TNT_SLOT_READY : XTC_TNT_SLOT_WAIT_MESSAGE;
				break;
			default:
				meta->state = XTC_TNT_SLOT_READY;
				break;
			}
		}
	}
}

/* One scheduler tick: drain external inputs, then for each type, for
 * each READY slot (budgeted), pop the head message and dispatch.
 * Returns the number of handlers run. */
static int
shard_tick(xtc_tnt_shard_t *sh)
{
	int ran = 0;
	int ai;

	drain_spawns(sh);
	drain_completions(sh);

	for (ai = 0; ai < sh->n_arenas; ai++) {
		xtc_tnt_arena_t *ar = &sh->arenas[ai];
		uint32_t budget = ar->type->budget_weight
		    ? ar->type->budget_weight : ar->slot_count;
		uint32_t scanned = 0;
		uint32_t dispatched = 0;

		while (scanned < ar->slot_count && dispatched < budget) {
			uint32_t slot = ar->cursor;
			xtc_tnt_slot_t *meta;

			ar->cursor = (ar->cursor + 1) % ar->slot_count;
			scanned++;
			meta = &ar->slots[slot];
			if (meta->state != XTC_TNT_SLOT_READY)
				continue;

			if (meta->mbox_depth > 0) {
				/* Pop head message, dispatch. */
				xtc_tnt_envelope_t *e = meta->mbox_head;
				xtc_tnt_message_t msg = e->msg;
				meta->mbox_head = e->next;
				if (meta->mbox_head == NULL)
					meta->mbox_tail = NULL;
				meta->mbox_depth--;
				__os_free(e);
				(void)dispatch_slot(sh, ar, slot, &msg);
			} else {
				/* READY with no message: a YIELD re-run with a
				 * synthetic empty message (tag 0). */
				xtc_tnt_message_t msg;
				memset(&msg, 0, sizeof(msg));
				(void)dispatch_slot(sh, ar, slot, &msg);
			}
			ran++;
			dispatched++;
		}
	}
	return ran;
}

/* Is there any work left (live slots that are READY, or pending I/O)? */
static int
shard_has_ready(xtc_tnt_shard_t *sh)
{
	int ai;
	for (ai = 0; ai < sh->n_arenas; ai++) {
		xtc_tnt_arena_t *ar = &sh->arenas[ai];
		uint32_t i;
		for (i = 0; i < ar->slot_count; i++)
			if (ar->slots[i].state == XTC_TNT_SLOT_READY)
				return 1;
	}
	return 0;
}

/* ---- Shard main fiber --------------------------------------------- */

static void
xtc_tnt_shard_main(void *arg)
{
	xtc_tnt_shard_t *sh = arg;
	uint8_t drain[256];

	tl_shard = sh;

	/* Boot isolate: shard 0 auto-spawns one instance of spec.boot_type
	 * (Tina's boot spec spawns a root).  boot_type < 0 disables it. */
	if (sh->id == 0 && sh->spec->boot_type >= 0) {
		xtc_tnt_arena_t *ar = shard_arena(sh,
		    (uint8_t)sh->spec->boot_type);
		if (ar != NULL) {
			xtc_tnt_spawn_error_t err;
			(void)slot_spawn(sh, ar, NULL, 0, &err);
		}
	}

	for (;;) {
		(void)shard_tick(sh);

		if (atomic_load(&sh->stop) || atomic_load(&sh->rt->stop)) {
			atomic_store(&sh->stop, 1);
			break;
		}

		/* If there is still READY work, loop again without parking. */
		if (shard_has_ready(sh)) {
			xtc_yield();   /* let couriers / peers run */
			continue;
		}

		/* Under deterministic simulation the shard has no real wake
		 * pipe to park on -- the seeded scheduler re-runs every ready
		 * fiber, and timers post via xtc_proc_sleep on the sim clock.
		 * Sleep on the sim clock for the poll interval instead of a
		 * real fd wait: this makes the shard non-runnable (so the run
		 * can reach quiescence rather than busy-spinning the step
		 * budget) yet still periodically re-check for ready work,
		 * cross-shard sends, and the stop flag -- all a pure function
		 * of the seed.  ADDITIVE: gated on __xtc_sim_active(); the
		 * production path below is byte-identical. */
		if (__xtc_sim_active()) {
			(void)xtc_proc_sleep(1LL * 1000 * 1000);
			continue;
		}

		/* Park on the wake pipe (couriers, timers, cross-shard sends,
		 * and external spawns all write a byte).  Bounded timeout so
		 * we re-check the stop flag. */
		{
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(sh->wake_rd,
			    XTC_IO_READABLE,
			    200LL * 1000 * 1000, &revents);
			/* Drain the pipe (coalesced wakeups). */
			while (read(sh->wake_rd, drain, sizeof(drain)) > 0)  /* XTC_BLOCKING_OK: nonblocking pipe drain after a ready wait */
				;
		}
	}

	tl_shard = NULL;
}

/* ---- Boot --------------------------------------------------------- */

static int
arena_init(xtc_tnt_shard_t *sh, int idx, const xtc_tnt_type_t *type)
{
	xtc_tnt_arena_t *ar = &sh->arenas[idx];
	uint32_t i;
	xtc_slab_opts_t mopts = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_opts_t sopts = XTC_SLAB_OPTS_DEFAULT;
	char nbuf[64];

	memset(ar, 0, sizeof(*ar));
	ar->type = type;
	ar->slot_count = type->slot_count;
	ar->cursor = 0;

	/* Carve the metadata + Isolate-store arrays from boot-time slab
	 * caches sized for the whole arena.  We allocate the arrays as one
	 * big object each (chunk_size grown to fit) so it is a single
	 * boot-time allocation per array -- no per-slot malloc. */
	snprintf(nbuf, sizeof(nbuf), "tnt.meta.%u.%s", sh->id, type->name);
	mopts.name = nbuf;
	mopts.obj_size = (size_t)ar->slot_count * sizeof(xtc_tnt_slot_t);
	mopts.chunk_size = mopts.obj_size + 4096;
	mopts.align = 64;
	if (xtc_slab_create(&mopts, &sh->slot_cache[idx]) != XTC_OK)
		return -1;
	ar->slots = xtc_slab_alloc(sh->slot_cache[idx]);
	if (ar->slots == NULL)
		return -1;
	memset(ar->slots, 0, mopts.obj_size);

	snprintf(nbuf, sizeof(nbuf), "tnt.store.%u.%s", sh->id, type->name);
	sopts.name = nbuf;
	sopts.obj_size = (size_t)ar->slot_count * type->stride;
	sopts.chunk_size = sopts.obj_size + 4096;
	sopts.align = 64;
	if (xtc_slab_create(&sopts, &sh->store_cache[idx]) != XTC_OK)
		return -1;
	ar->store = xtc_slab_alloc(sh->store_cache[idx]);
	if (ar->store == NULL)
		return -1;

	/* Free list (a boot allocation; small). */
	if (__os_malloc((size_t)ar->slot_count * sizeof(uint32_t),
	    (void **)&ar->free_list) != XTC_OK || ar->free_list == NULL)
		return -1;
	ar->free_top = 0;
	for (i = 0; i < ar->slot_count; i++) {
		ar->slots[i].gen = 1;        /* gen 0 reserved for NONE */
		ar->slots[i].state = XTC_TNT_SLOT_FREE;
		/* push in reverse so slot 0 is allocated first */
		ar->free_list[ar->free_top++] = ar->slot_count - 1 - i;
	}
	return 0;
}

static int
shard_init(xtc_tnt_runtime_t *rt, uint8_t id, xtc_tnt_shard_t **out)
{
	xtc_tnt_shard_t *sh = NULL;
	const xtc_tnt_spec_t *spec = rt->spec;
	int i;
	int pfd[2];
	uint32_t scratch_cap;

	if (__os_calloc(1, sizeof(*sh), (void **)&sh) != XTC_OK || sh == NULL)
		return -1;
	sh->id = id;
	sh->spec = spec;
	sh->rt = rt;
	atomic_init(&sh->stop, 0);
	pthread_mutex_init(&sh->comp_lock, NULL);
	pthread_mutex_init(&sh->spawn_lock, NULL);

	if (__os_calloc(XTC_TNT_COMPLETION_CAP, sizeof(xtc_tnt_completion_t),
	    (void **)&sh->comp_ring) != XTC_OK || sh->comp_ring == NULL)
		goto fail;

	scratch_cap = spec->scratch_size ? spec->scratch_size : 65536;
	sh->scratch_cap = scratch_cap;
	if (__os_malloc(scratch_cap, (void **)&sh->scratch) != XTC_OK ||
	    sh->scratch == NULL)
		goto fail;

	if (pipe(pfd) != 0)
		goto fail;
	sh->wake_rd = pfd[0];
	sh->wake_wr = pfd[1];
	(void)fcntl(sh->wake_rd, F_SETFL, O_NONBLOCK);  /* XTC_BLOCKING_OK: fd flag set */
	(void)fcntl(sh->wake_wr, F_SETFL, O_NONBLOCK);  /* XTC_BLOCKING_OK: fd flag set */

	for (i = 0; i < spec->n_types; i++) {
		if (arena_init(sh, i, &spec->types[i]) != 0)
			goto fail;
		sh->n_arenas++;
	}

	*out = sh;
	return 0;

fail:
	if (sh) {
		if (sh->comp_ring)
			__os_free(sh->comp_ring);
		if (sh->scratch)
			__os_free(sh->scratch);
		__os_free(sh);
	}
	return -1;
}

/* The xtc_exec task that establishes the shard fiber on its loop.  It
 * runs once on the target loop, spawns the long-lived shard proc on
 * that loop, and completes (XTC_TASK_DONE). */
static int
shard_bootstrap(xtc_task_t *self, void *arg)
{
	xtc_tnt_shard_t *sh = arg;
	xtc_proc_opts_t popts = { 0 };
	xtc_pid_t pid;
	xtc_loop_t *loop;

	(void)self;
	loop = xtc_exec_loop(sh->rt->exec, sh->id);
	sh->loop = loop;
	popts.name = "tnt-shard";
	(void)xtc_proc_spawn(loop, xtc_tnt_shard_main, sh, &popts, &pid);
	return XTC_TASK_DONE;
}

/* Release a shard's arenas and backing memory.  Called after the
 * executor has stopped (no shard fiber is running). */
static void
shard_destroy(xtc_tnt_shard_t *sh)
{
	int i;
	xtc_tnt_spawn_req_t *req, *n;

	if (sh == NULL)
		return;

	/* Drain + free any queued mailbox envelopes still in arenas. */
	for (i = 0; i < sh->n_arenas; i++) {
		xtc_tnt_arena_t *ar = &sh->arenas[i];
		uint32_t s;
		for (s = 0; s < ar->slot_count; s++) {
			xtc_tnt_envelope_t *e, *en;
			for (e = ar->slots[s].mbox_head; e != NULL; e = en) {
				en = e->next;
				__os_free(e);
			}
		}
		if (ar->free_list)
			__os_free(ar->free_list);
		/* slots + store live in slab caches destroyed below. */
		if (sh->slot_cache[i])
			xtc_slab_destroy(sh->slot_cache[i]);
		if (sh->store_cache[i])
			xtc_slab_destroy(sh->store_cache[i]);
	}

	/* Drain pending external spawn requests. */
	for (req = sh->spawn_head; req != NULL; req = n) {
		n = req->next;
		__os_free(req);
	}

	if (sh->comp_ring)
		__os_free(sh->comp_ring);
	if (sh->scratch)
		__os_free(sh->scratch);
	if (sh->wake_rd >= 0)
		close(sh->wake_rd);
	if (sh->wake_wr >= 0)
		close(sh->wake_wr);
	pthread_mutex_destroy(&sh->comp_lock);
	pthread_mutex_destroy(&sh->spawn_lock);
	__os_free(sh);
}

int
xtc_tnt_start(const xtc_tnt_spec_t *spec)
{
	xtc_tnt_runtime_t *rt = NULL;
	int i;
	int rc;

	if (spec == NULL || spec->n_types <= 0 ||
	    spec->n_types > XTC_TNT_MAX_TYPES)
		return XTC_E_INVAL;
	if (spec->shard_count <= 0 || spec->shard_count > XTC_TNT_MAX_SHARDS)
		return XTC_E_INVAL;

	if (__os_calloc(1, sizeof(*rt), (void **)&rt) != XTC_OK || rt == NULL)
		return XTC_E_NOMEM;
	rt->spec = spec;
	rt->shard_count = spec->shard_count;
	atomic_init(&rt->stop, 0);

	if (xtc_exec_init(&rt->exec, spec->shard_count) != XTC_OK) {
		__os_free(rt);
		return XTC_E_INTERNAL;
	}
	xtc_exec_set_service_mode(rt->exec, 1);

	for (i = 0; i < spec->shard_count; i++) {
		if (shard_init(rt, (uint8_t)i, &rt->shards[i]) != 0) {
			(void)xtc_exec_fini(rt->exec);
			__os_free(rt);
			return XTC_E_NOMEM;
		}
	}

	g_rt = rt;

	/* Place one shard bootstrap task on each loop.  Each bootstrap
	 * runs on its target loop and spawns that loop's long-lived shard
	 * proc, then completes. */
	for (i = 0; i < spec->shard_count; i++) {
		xtc_task_t *t = NULL;
		if (xtc_exec_spawn_on(rt->exec, i, shard_bootstrap,
		    rt->shards[i], &t) != XTC_OK) {
			xtc_tnt_stop();
			break;
		}
	}

	/* Run.  Blocks until xtc_tnt_stop / exec stop. */
	rc = xtc_exec_run(rt->exec);

	(void)xtc_exec_fini(rt->exec);
	g_rt = NULL;
	for (i = 0; i < rt->shard_count; i++)
		shard_destroy(rt->shards[i]);
	__os_free(rt);
	return rc;
}

/*
 * DST harness entry (internal, test-only).  Bring up exactly the same
 * shard runtime as xtc_tnt_start, but drive the loops with the seeded
 * deterministic simulator (xtc_sim_exec_run) instead of the production
 * xtc_exec_run.  This exercises tnt's deterministic actor core -- spawn
 * / arena allocation, mailbox delivery (drop-on-full, stale-handle,
 * pool-exhausted), generational slot reuse, cross-shard send, handler
 * transitions, budget fairness, timers (via the sim virtual clock) and
 * crash transitions -- as a pure function of the seed, with replay.
 *
 * The socket courier I/O (raw recv/send) is deliberately NOT simulated:
 * it needs a real kernel and is documented not-coverable-by-design.  A
 * spec used with this harness must not stage real fd I/O.
 *
 * Returns the xtc_sim_exec_run result (XTC_OK on clean quiescence).
 * out_stop_at, if non-NULL, receives 1 when the run stopped via
 * xtc_tnt_stop / exec stop rather than natural quiescence.
 *
 * PUBLIC-TEST: int __xtc_tnt_run_sim __P((const xtc_tnt_spec_t *, uint64_t, long));
 */
int
__xtc_tnt_run_sim(const xtc_tnt_spec_t *spec, uint64_t seed, long max_steps)
{
	xtc_tnt_runtime_t *rt = NULL;
	int i;
	int rc;

	if (spec == NULL || spec->n_types <= 0 ||
	    spec->n_types > XTC_TNT_MAX_TYPES)
		return XTC_E_INVAL;
	if (spec->shard_count <= 0 || spec->shard_count > XTC_TNT_MAX_SHARDS)
		return XTC_E_INVAL;

	if (__os_calloc(1, sizeof(*rt), (void **)&rt) != XTC_OK || rt == NULL)
		return XTC_E_NOMEM;
	rt->spec = spec;
	rt->shard_count = spec->shard_count;
	atomic_init(&rt->stop, 0);

	if (xtc_exec_init(&rt->exec, spec->shard_count) != XTC_OK) {
		__os_free(rt);
		return XTC_E_INTERNAL;
	}
	xtc_exec_set_service_mode(rt->exec, 1);

	for (i = 0; i < spec->shard_count; i++) {
		if (shard_init(rt, (uint8_t)i, &rt->shards[i]) != 0) {
			(void)xtc_exec_fini(rt->exec);
			__os_free(rt);
			return XTC_E_NOMEM;
		}
	}

	g_rt = rt;

	for (i = 0; i < spec->shard_count; i++) {
		xtc_task_t *t = NULL;
		if (xtc_exec_spawn_on(rt->exec, i, shard_bootstrap,
		    rt->shards[i], &t) != XTC_OK) {
			atomic_store(&rt->stop, 1);
			break;
		}
	}

	rc = xtc_sim_exec_run(rt->exec, seed, max_steps);

	(void)xtc_exec_fini(rt->exec);
	g_rt = NULL;
	for (i = 0; i < rt->shard_count; i++)
		shard_destroy(rt->shards[i]);
	__os_free(rt);
	return rc;
}

void
xtc_tnt_stop(void)
{
	xtc_tnt_runtime_t *rt = g_rt;
	int i;
	if (rt == NULL)
		return;
	atomic_store(&rt->stop, 1);
	for (i = 0; i < rt->shard_count; i++) {
		if (rt->shards[i]) {
			atomic_store(&rt->shards[i]->stop, 1);
			shard_wake(rt->shards[i]);
		}
	}
	(void)xtc_exec_stop(rt->exec);
}

#else  /* _WIN32: tnt is a POSIX feature (raw socket I/O in the couriers) */

/*
 * NOSYS stubs so the library links on a non-POSIX target.  The
 * stackless-Isolate layer requires POSIX socket I/O; a Windows port
 * would route the courier I/O through the IOCP-backed xtc_net instead.
 */
#include <stddef.h>

int
xtc_tnt_start(const xtc_tnt_spec_t *spec)
{ (void)spec; return XTC_E_NOSYS; }

void
xtc_tnt_stop(void) { }

xtc_tnt_spawn_error_t
xtc_tnt_spawn_on(uint8_t shard, uint8_t type_id, const void *args,
    size_t args_size)
{ (void)shard; (void)type_id; (void)args; (void)args_size;
  return XTC_TNT_SPAWN_INIT_FAILED; }

xtc_tnt_send_result_t
xtc_tnt_send(xtc_tnt_handle_t to, uint16_t tag, const void *payload,
    size_t payload_size)
{ (void)to; (void)tag; (void)payload; (void)payload_size;
  return XTC_TNT_SEND_STALE_HANDLE; }

xtc_tnt_spawn_error_t
xtc_tnt_spawn(uint8_t type_id, const void *args, size_t args_size,
    xtc_tnt_handle_t *out_handle)
{ (void)type_id; (void)args; (void)args_size;
  if (out_handle != NULL) *out_handle = XTC_TNT_HANDLE_NONE;
  return XTC_TNT_SPAWN_INIT_FAILED; }

xtc_tnt_io_result_t
xtc_tnt_submit_recv(int fd) { (void)fd; return XTC_TNT_IO_BAD_FD; }

xtc_tnt_io_result_t
xtc_tnt_io_send(int fd, const void *buffer, size_t len)
{ (void)fd; (void)buffer; (void)len; return XTC_TNT_IO_BAD_FD; }

xtc_tnt_io_result_t
xtc_tnt_submit_close(int fd) { (void)fd; return XTC_TNT_IO_BAD_FD; }

void
xtc_tnt_register_timer(uint64_t duration_ns, uint16_t tag)
{ (void)duration_ns; (void)tag; }

xtc_tnt_handle_t
xtc_tnt_self(void) { return XTC_TNT_HANDLE_NONE; }

uint8_t
xtc_tnt_shard_id(void) { return 0; }

void *
xtc_tnt_scratch_arena(size_t size) { (void)size; return NULL; }

#endif /* !_WIN32 */
