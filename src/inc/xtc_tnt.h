/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_tnt.h
 *	tnt -- a Tina-faithful, stackless Isolate layer (L4) built on
 *	top of the libxtc concurrency runtime.  See the Tina reference at
 *	github.com/pmbanugo/tina.
 *
 *	The decisive architectural choice is to map
 *	a Shard to one long-lived libxtc proc per xtc_exec loop, NOT an
 *	Isolate to a proc.  The shard proc owns a dense typed arena of
 *	stackless Isolate structs and runs Tina's dispatch loop:
 *
 *	    drain inbox
 *	    -> collect reactor completions
 *	    -> for each ready type, for each dispatchable slot (budgeted):
 *	         call handler -> interpret the returned transition/effect
 *	         (commit staged I/O, enqueue sends, set state).
 *
 *	Handlers NEVER block; they return a transition.  Isolates are
 *	stackless arena structs; only the shard is a fiber.
 *
 *	This header is the entire public surface.  All ambient ctx_*
 *	style calls (here named xtc_tnt_*) are valid only during an active
 *	handler invocation -- they resolve the current shard + turn
 *	frame from thread-local state, exactly as Tina's TinaContext.
 */

#ifndef XTC_TNT_H
#define XTC_TNT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Handles ------------------------------------------------------
 *
 * A 64-bit generational handle, bit-identical to Tina's layout:
 *
 *   +----------+------------+-------------+--------------+
 *   | shard_id |  type_id   | slot_index  |  generation  |
 *   |  (8 bit) |  (8 bit)   |  (20 bit)   |   (28 bit)   |
 *   +----------+------------+-------------+--------------+
 *
 * The generation is the key to safety: when an Isolate is torn down,
 * its slot's generation counter increments, so any handle still
 * referencing the old generation is recognisably stale.  A send to a
 * stale handle returns XTC_TNT_SEND_STALE_HANDLE rather than delivering to
 * whatever new Isolate now occupies the slot.
 */
typedef uint64_t xtc_tnt_handle_t;

#define XTC_TNT_HANDLE_NONE ((xtc_tnt_handle_t)0)

#define XTC_TNT_SHARD_BITS 8
#define XTC_TNT_TYPE_BITS  8
#define XTC_TNT_SLOT_BITS  20
#define XTC_TNT_GEN_BITS   28

#define XTC_TNT_SLOT_MAX   ((1u << XTC_TNT_SLOT_BITS) - 1u)
#define XTC_TNT_GEN_MASK   ((1u << XTC_TNT_GEN_BITS) - 1u)

static inline xtc_tnt_handle_t
xtc_tnt_handle_make(uint8_t shard, uint8_t type, uint32_t slot, uint32_t gen)
{
	return ((xtc_tnt_handle_t)shard << (XTC_TNT_TYPE_BITS + XTC_TNT_SLOT_BITS +
	                                XTC_TNT_GEN_BITS)) |
	       ((xtc_tnt_handle_t)type << (XTC_TNT_SLOT_BITS + XTC_TNT_GEN_BITS)) |
	       ((xtc_tnt_handle_t)(slot & XTC_TNT_SLOT_MAX) << XTC_TNT_GEN_BITS) |
	       ((xtc_tnt_handle_t)(gen & XTC_TNT_GEN_MASK));
}

static inline uint8_t
xtc_tnt_handle_shard(xtc_tnt_handle_t h)
{
	return (uint8_t)(h >> (XTC_TNT_TYPE_BITS + XTC_TNT_SLOT_BITS + XTC_TNT_GEN_BITS));
}

static inline uint8_t
xtc_tnt_handle_type(xtc_tnt_handle_t h)
{
	return (uint8_t)((h >> (XTC_TNT_SLOT_BITS + XTC_TNT_GEN_BITS)) & 0xffu);
}

static inline uint32_t
xtc_tnt_handle_slot(xtc_tnt_handle_t h)
{
	return (uint32_t)((h >> XTC_TNT_GEN_BITS) & XTC_TNT_SLOT_MAX);
}

static inline uint32_t
xtc_tnt_handle_gen(xtc_tnt_handle_t h)
{
	return (uint32_t)(h & XTC_TNT_GEN_MASK);
}

/* ---- Transitions --------------------------------------------------
 *
 * A handler returns a transition -- a small value describing what the
 * Isolate wants next, not how to schedule it.  The scheduler (the
 * shard) interprets it.  This is the core abstraction that lets the
 * effect interpreter be swapped for deterministic simulation later:
 * the Isolate code is identical in production and in DST.
 */
typedef enum xtc_tnt_transition_kind {
	XTC_TNT_DONE = 0,         /* clean exit -- deallocate me */
	XTC_TNT_YIELD,            /* run me again next tick */
	XTC_TNT_WAIT_MESSAGE,     /* park until my mailbox has a message */
	XTC_TNT_WAIT_IO,          /* staged I/O via xtc_tnt_submit_io -- park on it */
	XTC_TNT_CRASH             /* voluntary failure -- "let it crash" */
} xtc_tnt_transition_kind_t;

/* Fault reasons mirror Tina's Isolate_Fault_Reason. */
typedef enum xtc_tnt_fault_reason {
	XTC_TNT_FAULT_NONE = 0,
	XTC_TNT_FAULT_SPAWN_FAILED,
	XTC_TNT_FAULT_UNIMPLEMENTED_TRANSITION,
	XTC_TNT_FAULT_INIT_FAILED,
	XTC_TNT_FAULT_CONTRACT_VIOLATION
} xtc_tnt_fault_reason_t;

typedef struct xtc_tnt_transition {
	xtc_tnt_transition_kind_t kind;
	xtc_tnt_fault_reason_t    fault_reason;
} xtc_tnt_transition_t;

#define XTC_TNT_TRANSITION_DONE         ((xtc_tnt_transition_t){ XTC_TNT_DONE, XTC_TNT_FAULT_NONE })
#define XTC_TNT_TRANSITION_YIELD        ((xtc_tnt_transition_t){ XTC_TNT_YIELD, XTC_TNT_FAULT_NONE })
#define XTC_TNT_TRANSITION_WAIT_MESSAGE ((xtc_tnt_transition_t){ XTC_TNT_WAIT_MESSAGE, XTC_TNT_FAULT_NONE })
#define XTC_TNT_TRANSITION_WAIT_IO      ((xtc_tnt_transition_t){ XTC_TNT_WAIT_IO, XTC_TNT_FAULT_NONE })

static inline xtc_tnt_transition_t
xtc_tnt_transition_to_crash(xtc_tnt_fault_reason_t reason)
{
	xtc_tnt_transition_t t = { XTC_TNT_CRASH, reason };
	return t;
}

/* ---- Messages -----------------------------------------------------
 *
 * Every message is a fixed-size envelope with a tag discriminant and a
 * raw_union body: a user payload or an I/O completion.  Switch on
 * message->tag to determine which.
 */

/* Tag constants -- system tags are < XTC_TNT_USER_TAG_BASE; user tags
 * must be >= XTC_TNT_USER_TAG_BASE (matching Tina's 0x0040 base). */
#define XTC_TNT_USER_TAG_BASE 0x0040u

/* I/O completion tags (delivered by the effect interpreter / reactor,
 * never user-sendable).  Values match Tina's IO_TAG_* constants. */
#define XTC_TNT_IO_TAG_ACCEPT_COMPLETE 0x0012u
#define XTC_TNT_IO_TAG_CONNECT_COMPLETE 0x0013u
#define XTC_TNT_IO_TAG_SEND_COMPLETE   0x0014u
#define XTC_TNT_IO_TAG_RECV_COMPLETE   0x0015u
#define XTC_TNT_IO_TAG_CLOSE_COMPLETE  0x0018u

/* System tags. */
#define XTC_TNT_TAG_TIMER    0x0002u
#define XTC_TNT_TAG_SHUTDOWN 0x0003u

#define XTC_TNT_MAX_PAYLOAD_SIZE  96
#define XTC_TNT_MAX_INIT_ARGS_SIZE 64

typedef struct xtc_tnt_message {
	uint16_t tag;
	union {
		/* User / system message body.  payload is placed after an
		 * 8-byte aligned prefix (source + size + pad) so that, with
		 * the union itself 8-aligned, payload starts on an 8-byte
		 * boundary -- a u32/u64 in the payload loads aligned. */
		struct {
			xtc_tnt_handle_t source;        /* offset 0 */
			uint16_t     payload_size;  /* offset 8 */
			uint8_t      _pad[6];       /* pad to 16 */
			uint8_t      payload[XTC_TNT_MAX_PAYLOAD_SIZE]; /* offset 16 */
		} user;
		/* I/O completion body. */
		struct {
			int      fd;        /* which fd completed (or new fd) */
			int32_t  result;    /* bytes transferred, or -errno */
			void    *buffer;    /* reactor buffer (recv only) */
			uint32_t buffer_len;
		} io;
	} body;
} xtc_tnt_message_t;

/* ---- Isolate type descriptor --------------------------------------
 *
 * Three artifacts per Isolate type (Tina's IsolateTypeDescriptor): the
 * struct stride, an init_fn (called once on spawn, returns the initial
 * transition), and a handler_fn (called on every message / completion,
 * returns the next transition).  Both functions receive a rawptr to
 * the Isolate's slot because the scheduler operates on heterogeneous
 * typed arenas; use xtc_tnt_self_as to cast.
 */
typedef xtc_tnt_transition_t (*xtc_tnt_init_fn)(void *self, const void *args,
                                        size_t args_size);
typedef xtc_tnt_transition_t (*xtc_tnt_handler_fn)(void *self, xtc_tnt_message_t *msg);

typedef struct xtc_tnt_type {
	uint8_t        id;                  /* 0..255 -- index in spec.types */
	const char    *name;                /* for diagnostics */
	uint32_t       slot_count;          /* arena capacity for this type */
	size_t         stride;              /* sizeof(the Isolate struct) */
	size_t         working_memory_size; /* per-slot working arena bytes */
	uint32_t       mailbox_capacity;    /* bounded mailbox depth */
	uint32_t       budget_weight;       /* max slots dispatched per tick */
	xtc_tnt_init_fn    init_fn;
	xtc_tnt_handler_fn handler_fn;
} xtc_tnt_type_t;

/* ---- System spec --------------------------------------------------
 *
 * Tina's SystemSpec, trimmed to this slice.  At boot xtc_tnt_start carves
 * every arena from boot-time slab caches (one per type per shard) and
 * never mallocs on the hot path again (disciplinary, per the doc).
 */
typedef struct xtc_tnt_spec {
	const char       *name;
	const xtc_tnt_type_t *types;        /* descriptor table */
	int               n_types;
	int               shard_count;  /* number of shards (loops) */
	uint32_t          scratch_size; /* per-shard turn scratch arena bytes */
	uint32_t          recv_buf_size;/* per-recv reactor buffer bytes */
	/* Optional: a type id to auto-spawn one instance of on shard 0
	 * once the runtime is up (Tina's boot spec spawns a root).  Set to
	 * a negative value (default after memset) to disable; the canonical
	 * use is a "driver" or "root supervisor" isolate.  -1 = none. */
	int               boot_type;
} xtc_tnt_spec_t;

/* ---- Send results -------------------------------------------------
 *
 * Tina's Send_Result.  The sender decides what to do -- the layer
 * never auto-retries or grows the mailbox.
 */
typedef enum xtc_tnt_send_result {
	XTC_TNT_SEND_OK = 0,            /* enqueued in the target mailbox */
	XTC_TNT_SEND_MAILBOX_FULL,      /* target mailbox at capacity -- DROPPED */
	XTC_TNT_SEND_POOL_EXHAUSTED,    /* no free envelope -- DROPPED */
	XTC_TNT_SEND_STALE_HANDLE       /* target dead / generation mismatch */
} xtc_tnt_send_result_t;

/* Spawn outcome. */
typedef enum xtc_tnt_spawn_error {
	XTC_TNT_SPAWN_OK = 0,
	XTC_TNT_SPAWN_ARENA_FULL,
	XTC_TNT_SPAWN_TYPE_NOT_ALLOCATED,
	XTC_TNT_SPAWN_INIT_FAILED
} xtc_tnt_spawn_error_t;

/* I/O submit results. */
typedef enum xtc_tnt_io_result {
	XTC_TNT_IO_OK = 0,
	XTC_TNT_IO_TOO_MANY,        /* turn frame staging slots exhausted */
	XTC_TNT_IO_BAD_FD
} xtc_tnt_io_result_t;

/* ---- Ambient ctx_* API (valid only during a handler) -------------- */

/* Messaging.  tag must be >= XTC_TNT_USER_TAG_BASE.  Payload max 96 bytes.
 * Drop-on-full with sender feedback, matching Tina's .mailbox_full. */
xtc_tnt_send_result_t xtc_tnt_send(xtc_tnt_handle_t to, uint16_t tag,
                           const void *payload, size_t payload_size);

/* Spawn a new Isolate of type_id on the CURRENT shard.  The child's
 * init_fn runs immediately (not deferred).  Returns the new handle in
 * *out_handle on success. */
xtc_tnt_spawn_error_t xtc_tnt_spawn(uint8_t type_id, const void *args,
                            size_t args_size, xtc_tnt_handle_t *out_handle);

/* I/O effects.  These stage an operation into the current turn frame;
 * the shard commits it into xtc_aio / the net layer only if the
 * handler returns XTC_TNT_WAIT_IO.  This is Tina's stage-then-commit. */

/* Stage a recv on fd into a reactor buffer; on completion the Isolate
 * receives a XTC_TNT_IO_TAG_RECV_COMPLETE message. */
xtc_tnt_io_result_t xtc_tnt_submit_recv(int fd);

/* Stage a send of buffer[0..len) on fd; buffer must live inside the
 * Isolate struct (it is read at commit time, after the handler
 * returns).  On completion: XTC_TNT_IO_TAG_SEND_COMPLETE. */
xtc_tnt_io_result_t xtc_tnt_io_send(int fd, const void *buffer, size_t len);

/* Stage a close of fd; on completion: XTC_TNT_IO_TAG_CLOSE_COMPLETE. */
xtc_tnt_io_result_t xtc_tnt_submit_close(int fd);

/* Register a one-shot timer; after duration_ns the Isolate receives a
 * message with the given tag. */
void xtc_tnt_register_timer(uint64_t duration_ns, uint16_t tag);

/* The current Isolate's identity. */
xtc_tnt_handle_t xtc_tnt_self(void);

/* The current shard's 0-based id. */
uint8_t xtc_tnt_shard_id(void);

/* The shard-wide scratch arena, reset before every handler call.  For
 * per-turn temporaries only; do not retain across handler returns.
 * Returns a bump pointer of `size` bytes, or NULL if exhausted. */
void *xtc_tnt_scratch_arena(size_t size);

/* ---- Ergonomic helpers -------------------------------------------- */

/* Debug-checked cast from the rawptr self to a typed Isolate pointer.
 * In a debug build this validates the stride matches sizeof(T). */
#define xtc_tnt_self_as(T, self_raw) ((T *)(self_raw))

/* Cast a message payload to a typed pointer. */
#define xtc_tnt_payload_as(T, msg) ((T *)((msg)->body.user.payload))

/* ---- Boot ---------------------------------------------------------
 *
 * Validate the spec, carve all arenas from boot-time slab caches, pin
 * one shard proc per loop, and enter the run loop.  Blocks until the
 * system stops (xtc_tnt_stop from any thread, or all shards idle).  Returns
 * XTC_OK-style 0 on clean shutdown, negative on a boot failure.
 */
int xtc_tnt_start(const xtc_tnt_spec_t *spec);

/* Request the running system to stop (kicks every shard).  Safe to
 * call from a signal handler or another thread. */
void xtc_tnt_stop(void);

/* Spawn an Isolate from OUTSIDE the shard fibers (e.g. from main before
 * xtc_tnt_start, or from a non-isolate listener).  Routes the spawn onto
 * the target shard and runs the init synchronously on that shard's next
 * tick.  shard is 0..shard_count-1.  Thread-safe. */
xtc_tnt_spawn_error_t xtc_tnt_spawn_on(uint8_t shard, uint8_t type_id,
                               const void *args, size_t args_size);

#ifdef __cplusplus
}
#endif

#endif /* XTC_TNT_H */
