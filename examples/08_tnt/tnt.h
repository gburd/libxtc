/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/08_tnt/tnt.h
 *	tnt -- a Tina-faithful Isolate layer built on top of the libxtc
 *	concurrency runtime.  See docs/M_TINA_LAYER.md (authoritative)
 *	and the Tina reference at github.com/pmbanugo/tina.
 *
 *	The decisive architectural choice (M_TINA_LAYER.md) is to map
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
 *	style calls (here named tnt_*) are valid only during an active
 *	handler invocation -- they resolve the current shard + turn
 *	frame from thread-local state, exactly as Tina's TinaContext.
 */

#ifndef TNT_H
#define TNT_H

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
 * stale handle returns TNT_SEND_STALE_HANDLE rather than delivering to
 * whatever new Isolate now occupies the slot.
 */
typedef uint64_t tnt_handle_t;

#define TNT_HANDLE_NONE ((tnt_handle_t)0)

#define TNT_SHARD_BITS 8
#define TNT_TYPE_BITS  8
#define TNT_SLOT_BITS  20
#define TNT_GEN_BITS   28

#define TNT_SLOT_MAX   ((1u << TNT_SLOT_BITS) - 1u)
#define TNT_GEN_MASK   ((1u << TNT_GEN_BITS) - 1u)

static inline tnt_handle_t
tnt_handle_make(uint8_t shard, uint8_t type, uint32_t slot, uint32_t gen)
{
	return ((tnt_handle_t)shard << (TNT_TYPE_BITS + TNT_SLOT_BITS +
	                                TNT_GEN_BITS)) |
	       ((tnt_handle_t)type << (TNT_SLOT_BITS + TNT_GEN_BITS)) |
	       ((tnt_handle_t)(slot & TNT_SLOT_MAX) << TNT_GEN_BITS) |
	       ((tnt_handle_t)(gen & TNT_GEN_MASK));
}

static inline uint8_t
tnt_handle_shard(tnt_handle_t h)
{
	return (uint8_t)(h >> (TNT_TYPE_BITS + TNT_SLOT_BITS + TNT_GEN_BITS));
}

static inline uint8_t
tnt_handle_type(tnt_handle_t h)
{
	return (uint8_t)((h >> (TNT_SLOT_BITS + TNT_GEN_BITS)) & 0xffu);
}

static inline uint32_t
tnt_handle_slot(tnt_handle_t h)
{
	return (uint32_t)((h >> TNT_GEN_BITS) & TNT_SLOT_MAX);
}

static inline uint32_t
tnt_handle_gen(tnt_handle_t h)
{
	return (uint32_t)(h & TNT_GEN_MASK);
}

/* ---- Transitions --------------------------------------------------
 *
 * A handler returns a transition -- a small value describing what the
 * Isolate wants next, not how to schedule it.  The scheduler (the
 * shard) interprets it.  This is the core abstraction that lets the
 * effect interpreter be swapped for deterministic simulation later:
 * the Isolate code is identical in production and in DST.
 */
typedef enum tnt_transition_kind {
	TNT_DONE = 0,         /* clean exit -- deallocate me */
	TNT_YIELD,            /* run me again next tick */
	TNT_WAIT_MESSAGE,     /* park until my mailbox has a message */
	TNT_WAIT_IO,          /* staged I/O via tnt_submit_io -- park on it */
	TNT_CRASH             /* voluntary failure -- "let it crash" */
} tnt_transition_kind_t;

/* Fault reasons mirror Tina's Isolate_Fault_Reason. */
typedef enum tnt_fault_reason {
	TNT_FAULT_NONE = 0,
	TNT_FAULT_SPAWN_FAILED,
	TNT_FAULT_UNIMPLEMENTED_TRANSITION,
	TNT_FAULT_INIT_FAILED,
	TNT_FAULT_CONTRACT_VIOLATION
} tnt_fault_reason_t;

typedef struct tnt_transition {
	tnt_transition_kind_t kind;
	tnt_fault_reason_t    fault_reason;
} tnt_transition_t;

#define TNT_TRANSITION_DONE         ((tnt_transition_t){ TNT_DONE, TNT_FAULT_NONE })
#define TNT_TRANSITION_YIELD        ((tnt_transition_t){ TNT_YIELD, TNT_FAULT_NONE })
#define TNT_TRANSITION_WAIT_MESSAGE ((tnt_transition_t){ TNT_WAIT_MESSAGE, TNT_FAULT_NONE })
#define TNT_TRANSITION_WAIT_IO      ((tnt_transition_t){ TNT_WAIT_IO, TNT_FAULT_NONE })

static inline tnt_transition_t
tnt_transition_to_crash(tnt_fault_reason_t reason)
{
	tnt_transition_t t = { TNT_CRASH, reason };
	return t;
}

/* ---- Messages -----------------------------------------------------
 *
 * Every message is a fixed-size envelope with a tag discriminant and a
 * raw_union body: a user payload or an I/O completion.  Switch on
 * message->tag to determine which.
 */

/* Tag constants -- system tags are < TNT_USER_TAG_BASE; user tags
 * must be >= TNT_USER_TAG_BASE (matching Tina's 0x0040 base). */
#define TNT_USER_TAG_BASE 0x0040u

/* I/O completion tags (delivered by the effect interpreter / reactor,
 * never user-sendable).  Values match Tina's IO_TAG_* constants. */
#define TNT_IO_TAG_ACCEPT_COMPLETE 0x0012u
#define TNT_IO_TAG_CONNECT_COMPLETE 0x0013u
#define TNT_IO_TAG_SEND_COMPLETE   0x0014u
#define TNT_IO_TAG_RECV_COMPLETE   0x0015u
#define TNT_IO_TAG_CLOSE_COMPLETE  0x0018u

/* System tags. */
#define TNT_TAG_TIMER    0x0002u
#define TNT_TAG_SHUTDOWN 0x0003u

#define TNT_MAX_PAYLOAD_SIZE  96
#define TNT_MAX_INIT_ARGS_SIZE 64

typedef struct tnt_message {
	uint16_t tag;
	union {
		/* User / system message body.  payload is placed after an
		 * 8-byte aligned prefix (source + size + pad) so that, with
		 * the union itself 8-aligned, payload starts on an 8-byte
		 * boundary -- a u32/u64 in the payload loads aligned. */
		struct {
			tnt_handle_t source;        /* offset 0 */
			uint16_t     payload_size;  /* offset 8 */
			uint8_t      _pad[6];       /* pad to 16 */
			uint8_t      payload[TNT_MAX_PAYLOAD_SIZE]; /* offset 16 */
		} user;
		/* I/O completion body. */
		struct {
			int      fd;        /* which fd completed (or new fd) */
			int32_t  result;    /* bytes transferred, or -errno */
			void    *buffer;    /* reactor buffer (recv only) */
			uint32_t buffer_len;
		} io;
	} body;
} tnt_message_t;

/* ---- Isolate type descriptor --------------------------------------
 *
 * Three artifacts per Isolate type (Tina's IsolateTypeDescriptor): the
 * struct stride, an init_fn (called once on spawn, returns the initial
 * transition), and a handler_fn (called on every message / completion,
 * returns the next transition).  Both functions receive a rawptr to
 * the Isolate's slot because the scheduler operates on heterogeneous
 * typed arenas; use tnt_self_as to cast.
 */
typedef tnt_transition_t (*tnt_init_fn)(void *self, const void *args,
                                        size_t args_size);
typedef tnt_transition_t (*tnt_handler_fn)(void *self, tnt_message_t *msg);

typedef struct tnt_type {
	uint8_t        id;                  /* 0..255 -- index in spec.types */
	const char    *name;                /* for diagnostics */
	uint32_t       slot_count;          /* arena capacity for this type */
	size_t         stride;              /* sizeof(the Isolate struct) */
	size_t         working_memory_size; /* per-slot working arena bytes */
	uint32_t       mailbox_capacity;    /* bounded mailbox depth */
	uint32_t       budget_weight;       /* max slots dispatched per tick */
	tnt_init_fn    init_fn;
	tnt_handler_fn handler_fn;
} tnt_type_t;

/* ---- System spec --------------------------------------------------
 *
 * Tina's SystemSpec, trimmed to this slice.  At boot tnt_start carves
 * every arena from boot-time slab caches (one per type per shard) and
 * never mallocs on the hot path again (disciplinary, per the doc).
 */
typedef struct tnt_spec {
	const char       *name;
	const tnt_type_t *types;        /* descriptor table */
	int               n_types;
	int               shard_count;  /* number of shards (loops) */
	uint32_t          scratch_size; /* per-shard turn scratch arena bytes */
	uint32_t          recv_buf_size;/* per-recv reactor buffer bytes */
	/* Optional: a type id to auto-spawn one instance of on shard 0
	 * once the runtime is up (Tina's boot spec spawns a root).  Set to
	 * a negative value (default after memset) to disable; the canonical
	 * use is a "driver" or "root supervisor" isolate.  -1 = none. */
	int               boot_type;
} tnt_spec_t;

/* ---- Send results -------------------------------------------------
 *
 * Tina's Send_Result.  The sender decides what to do -- the layer
 * never auto-retries or grows the mailbox.
 */
typedef enum tnt_send_result {
	TNT_SEND_OK = 0,            /* enqueued in the target mailbox */
	TNT_SEND_MAILBOX_FULL,      /* target mailbox at capacity -- DROPPED */
	TNT_SEND_POOL_EXHAUSTED,    /* no free envelope -- DROPPED */
	TNT_SEND_STALE_HANDLE       /* target dead / generation mismatch */
} tnt_send_result_t;

/* Spawn outcome. */
typedef enum tnt_spawn_error {
	TNT_SPAWN_OK = 0,
	TNT_SPAWN_ARENA_FULL,
	TNT_SPAWN_TYPE_NOT_ALLOCATED,
	TNT_SPAWN_INIT_FAILED
} tnt_spawn_error_t;

/* I/O submit results. */
typedef enum tnt_io_result {
	TNT_IO_OK = 0,
	TNT_IO_TOO_MANY,        /* turn frame staging slots exhausted */
	TNT_IO_BAD_FD
} tnt_io_result_t;

/* ---- Ambient ctx_* API (valid only during a handler) -------------- */

/* Messaging.  tag must be >= TNT_USER_TAG_BASE.  Payload max 96 bytes.
 * Drop-on-full with sender feedback, matching Tina's .mailbox_full. */
tnt_send_result_t tnt_send(tnt_handle_t to, uint16_t tag,
                           const void *payload, size_t payload_size);

/* Spawn a new Isolate of type_id on the CURRENT shard.  The child's
 * init_fn runs immediately (not deferred).  Returns the new handle in
 * *out_handle on success. */
tnt_spawn_error_t tnt_spawn(uint8_t type_id, const void *args,
                            size_t args_size, tnt_handle_t *out_handle);

/* I/O effects.  These stage an operation into the current turn frame;
 * the shard commits it into xtc_aio / the net layer only if the
 * handler returns TNT_WAIT_IO.  This is Tina's stage-then-commit. */

/* Stage a recv on fd into a reactor buffer; on completion the Isolate
 * receives a TNT_IO_TAG_RECV_COMPLETE message. */
tnt_io_result_t tnt_submit_recv(int fd);

/* Stage a send of buffer[0..len) on fd; buffer must live inside the
 * Isolate struct (it is read at commit time, after the handler
 * returns).  On completion: TNT_IO_TAG_SEND_COMPLETE. */
tnt_io_result_t tnt_io_send(int fd, const void *buffer, size_t len);

/* Stage a close of fd; on completion: TNT_IO_TAG_CLOSE_COMPLETE. */
tnt_io_result_t tnt_submit_close(int fd);

/* Register a one-shot timer; after duration_ns the Isolate receives a
 * message with the given tag. */
void tnt_register_timer(uint64_t duration_ns, uint16_t tag);

/* The current Isolate's identity. */
tnt_handle_t tnt_self(void);

/* The current shard's 0-based id. */
uint8_t tnt_shard_id(void);

/* The shard-wide scratch arena, reset before every handler call.  For
 * per-turn temporaries only; do not retain across handler returns.
 * Returns a bump pointer of `size` bytes, or NULL if exhausted. */
void *tnt_scratch_arena(size_t size);

/* ---- Ergonomic helpers -------------------------------------------- */

/* Debug-checked cast from the rawptr self to a typed Isolate pointer.
 * In a debug build this validates the stride matches sizeof(T). */
#define tnt_self_as(T, self_raw) ((T *)(self_raw))

/* Cast a message payload to a typed pointer. */
#define tnt_payload_as(T, msg) ((T *)((msg)->body.user.payload))

/* ---- Boot ---------------------------------------------------------
 *
 * Validate the spec, carve all arenas from boot-time slab caches, pin
 * one shard proc per loop, and enter the run loop.  Blocks until the
 * system stops (tnt_stop from any thread, or all shards idle).  Returns
 * XTC_OK-style 0 on clean shutdown, negative on a boot failure.
 */
int tnt_start(const tnt_spec_t *spec);

/* Request the running system to stop (kicks every shard).  Safe to
 * call from a signal handler or another thread. */
void tnt_stop(void);

/* Spawn an Isolate from OUTSIDE the shard fibers (e.g. from main before
 * tnt_start, or from a non-isolate listener).  Routes the spawn onto
 * the target shard and runs the init synchronously on that shard's next
 * tick.  shard is 0..shard_count-1.  Thread-safe. */
tnt_spawn_error_t tnt_spawn_on(uint8_t shard, uint8_t type_id,
                               const void *args, size_t args_size);

#ifdef __cplusplus
}
#endif

#endif /* TNT_H */
