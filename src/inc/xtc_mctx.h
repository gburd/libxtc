/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_mctx.h
 *	Memory contexts: hierarchical allocation pools with parent-
 *	tracked lifetime and bulk reset/destroy.
 *
 *	A context owns:
 *	  - all allocations made within it (chained on a free list);
 *	  - zero or more child contexts (siblings of each other);
 *	  - optional "before-destroy" callbacks for non-memory cleanup.
 *
 *	Destroying a context destroys its children first, runs the
 *	cleanup callbacks bottom-up, then frees all chunks.  Reset
 *	frees chunks but keeps children alive.
 *
 *	M11 deliberately ships a simple-but-correct allocator first
 *	(every allocation = malloc + chain).  M11.5 will swap in slab
 *	caches for fixed-size hot paths and arena-free for large
 *	short-lived contexts.  The public API doesn't change.
 *
 *	Thread-safety: each context carries an optional pthread mutex.
 *	The default factory makes contexts unlocked for the per-loop
 *	(single-thread) common case; a flag enables locking for
 *	contexts that legitimately span threads.
 */

#ifndef XTC_MCTX_H
#define XTC_MCTX_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_mctx xtc_mctx_t;

typedef enum xtc_mctx_flags {
	XTC_MCTX_DEFAULT      = 0,
	XTC_MCTX_THREAD_SAFE  = 1u << 0   /* internally locked */
} xtc_mctx_flags_t;

typedef void (*xtc_mctx_cleanup_fn)(void *user);

/*
 * PUBLIC: int     xtc_mctx_create __P((xtc_mctx_t *, const char *, unsigned, xtc_mctx_t **));
 * PUBLIC: void    xtc_mctx_destroy __P((xtc_mctx_t *));
 * PUBLIC: void    xtc_mctx_reset __P((xtc_mctx_t *));
 *
 * PUBLIC: void   *xtc_mctx_alloc __P((xtc_mctx_t *, size_t));
 * PUBLIC: void   *xtc_mctx_calloc __P((xtc_mctx_t *, size_t, size_t));
 * PUBLIC: void   *xtc_mctx_strdup __P((xtc_mctx_t *, const char *));
 * PUBLIC: void    xtc_mctx_free __P((xtc_mctx_t *, void *));
 *
 * PUBLIC: int     xtc_mctx_register_cleanup __P((xtc_mctx_t *, xtc_mctx_cleanup_fn, void *));
 *
 * PUBLIC: const char *xtc_mctx_name __P((const xtc_mctx_t *));
 * PUBLIC: size_t      xtc_mctx_total_bytes __P((const xtc_mctx_t *));
 * PUBLIC: size_t      xtc_mctx_total_chunks __P((const xtc_mctx_t *));
 */

/* Create a child context.  parent==NULL produces a root context.
 * name is copied for diagnostics.  flags = bitmask of XTC_MCTX_*. */
XTC_API int     xtc_mctx_create(xtc_mctx_t *parent, const char *name,
                                unsigned flags, xtc_mctx_t **out);

/* Destroy a context: recursively destroys children, runs cleanups
 * bottom-up, frees all chunks.  Detaches from parent. */
XTC_API void    xtc_mctx_destroy(xtc_mctx_t *m);

/* Free all allocations and run cleanups, but keep the context (and
 * its children) usable.  Useful for per-iteration scratch contexts. */
XTC_API void    xtc_mctx_reset(xtc_mctx_t *m);

/* Allocate within the context.  Returns NULL on failure. */
XTC_API void   *xtc_mctx_alloc(xtc_mctx_t *m, size_t size);
XTC_API void   *xtc_mctx_calloc(xtc_mctx_t *m, size_t n, size_t size);

/* Strdup into the context.  Lives until reset/destroy. */
XTC_API void   *xtc_mctx_strdup(xtc_mctx_t *m, const char *s);

/* Free a single allocation early.  Optional -- most code just lets
 * destroy/reset reclaim. */
XTC_API void    xtc_mctx_free(xtc_mctx_t *m, void *p);

/* Register a cleanup callback.  Runs at destroy/reset time, before
 * the chunks are freed.  Multiple callbacks run in LIFO order.
 *
 * The context's lock (if XTC_MCTX_THREAD_SAFE) is NOT held while the
 * callback runs, so a callback may call back into its own context --
 * xtc_mctx_total_bytes/_total_chunks, or an alloc.  What it must NOT do
 * is reset or destroy the very context that is running it (that would
 * recurse into the same teardown).  Callbacks are detached before they
 * run, so each fires exactly once even if the context is reset
 * concurrently; register again if the next reset should call it too. */
XTC_API int     xtc_mctx_register_cleanup(xtc_mctx_t *m,
                                          xtc_mctx_cleanup_fn fn, void *user);

XTC_API const char *xtc_mctx_name(const xtc_mctx_t *m);
XTC_API size_t      xtc_mctx_total_bytes(const xtc_mctx_t *m);
XTC_API size_t      xtc_mctx_total_chunks(const xtc_mctx_t *m);

/* ---- arena groups: wholesale shared-state discard on kill ----
 *
 * The safe way to force-kill a fiber that mutates state OTHER fibers
 * keep using.  xtc_proc(3) documents why a plain async kill is unsafe
 * there: releasing a lock does not undo a half-finished mutation, and a
 * single address space gives no smaller recovery boundary than the
 * process -- so the honest escalation is "take a larger unit down and
 * rebuild its shared state from a durable log," the analogue of a
 * crash-only database reinitialising shared memory.
 *
 * An arena group makes that unit smaller than the whole process.  It
 * binds THREE things that already exist -- a set of fibers, one
 * memory context (the arena), and the async-kill primitive -- into one
 * recovery boundary:
 *
 *   - Every allocation the member fibers make for their SHARED state
 *     goes in the group's arena (xtc_arena_group_mctx()).
 *   - xtc_arena_group_discard() kills every member with a deadline,
 *     waits until they are ALL confirmed gone, and only THEN resets the
 *     arena wholesale.  Because no member is alive when the reset runs,
 *     nothing can be mid-mutation of the state being discarded -- the
 *     torn WAL record / half-updated page is thrown away with the
 *     arena, exactly as it would be on a process restart.
 *
 * This does NOT make an arbitrary mid-critical-section kill safe on its
 * own; safety comes from the two-phase order (all members dead, THEN
 * discard) plus the caller rebuilding from its log afterward.  It is
 * the group analogue of the whole-process fail-stop, at group
 * granularity.
 *
 * Members are the fibers registered with xtc_arena_group_add (a fiber
 * adds ITSELF, so ownership is unambiguous).  A member that exits on
 * its own is reaped from the group lazily (a dead pid is skipped at
 * discard time).  The group's arena is a normal xtc_mctx_t and may have
 * children; reset discards them too.
 *
 * JOIN/DISCARD RACE -- THE GROUP IS SEALED WHILE A DISCARD RUNS.
 * discard() must kill a FIXED cohort, and killing yields, so the window
 * between "snapshot the members" and "reset the arena" is long.  A
 * fiber joining inside that window would be neither killed (it is not
 * in the snapshot) nor safe (the reset wipes memory it just allocated).
 * So discard SEALS the group in the same lock hold that takes the
 * snapshot: from then until the discard reaches its verdict,
 * xtc_arena_group_add fails with XTC_E_AGAIN and MUST NOT touch the
 * arena.  A caller that gets XTC_E_AGAIN is racing a teardown of the
 * state it wanted to share; the correct response is to back off and
 * retry the join (succeeding once the discard finishes -- the group is
 * reusable by a fresh cohort), never to proceed with the arena.
 *
 * A MEMBER IS "GONE" ONLY ONCE IT IS REAPED, not merely once it is no
 * longer alive: a proc clears its alive flag before running its at-exit
 * callbacks, and those callbacks can still touch arena memory.  discard
 * waits for the pid to disappear from the proc table entirely, which is
 * the first moment nothing of the member can reach the arena.
 */
typedef struct xtc_arena_group xtc_arena_group_t;

/*
 * PUBLIC: int     xtc_arena_group_create __P((const char *, xtc_arena_group_t **));
 * PUBLIC: void    xtc_arena_group_destroy __P((xtc_arena_group_t *));
 * PUBLIC: xtc_mctx_t *xtc_arena_group_mctx __P((xtc_arena_group_t *));
 * PUBLIC: int     xtc_arena_group_add __P((xtc_arena_group_t *));
 * PUBLIC: int     xtc_arena_group_discard __P((xtc_arena_group_t *, int, int64_t, int *));
 * PUBLIC: int     xtc_arena_group_size __P((xtc_arena_group_t *));
 */

/* Create an empty group with a fresh thread-safe arena (members run on
 * different carriers, so the arena is internally locked).  `name` is
 * copied for diagnostics.  XTC_E_INVAL on NULL out, XTC_E_NOMEM on OOM. */
XTC_API int     xtc_arena_group_create(const char *name,
                                       xtc_arena_group_t **out);

/* Destroy the group and its arena.  Does NOT kill members -- call
 * discard first if any may still be alive; this just frees the group
 * and the arena's memory.  NULL-safe. */
XTC_API void    xtc_arena_group_destroy(xtc_arena_group_t *g);

/* The group's arena.  Allocate the members' SHARED state here so it can
 * be discarded wholesale.  Valid until xtc_arena_group_destroy. */
XTC_API xtc_mctx_t *xtc_arena_group_mctx(xtc_arena_group_t *g);

/* Register the CALLING fiber as a member.  Must be called from a proc
 * (xtc_self() != NONE); XTC_E_INVAL otherwise.  Idempotent per pid.
 * XTC_E_NOMEM if the member list cannot grow.
 *
 * XTC_E_AGAIN if a discard is in flight (the group is sealed): the
 * arena is about to be reset wholesale, so the caller was NOT admitted
 * and must NOT allocate in or read the arena.  Back off and retry -- the
 * seal lifts when the discard reaches its verdict, and the group is then
 * reusable by a fresh cohort. */
XTC_API int     xtc_arena_group_add(xtc_arena_group_t *g);

/* Discard the group: xtc_exit_pid_deadline every live member with
 * `reason`, wait up to `timeout_ns` PER member for it to go, then --
 * only if ALL members are confirmed gone -- reset the arena, throwing
 * away every allocation made through it.
 *
 * Joins are SEALED OFF for the whole call (see above): the cohort that
 * is killed is exactly the membership at entry, and no fiber can slip
 * into the group between the snapshot and the reset.
 *
 * *out_all_gone (may be NULL) is set to 1 if every member terminated
 * AND was reaped (its at-exit callbacks have finished) and the arena was
 * reset, or 0 if at least one member was still alive or still unwinding
 * at its deadline (a member wedged inside xtc_uncancelable with the
 * kill deferred, or one stuck in an at-exit callback).  In the 0 case
 * the arena is NOT reset -- discarding state a live fiber may still
 * touch is exactly the corruption this primitive exists to avoid -- and
 * the caller should escalate to the whole-process fail-stop.  Returns
 * XTC_OK once a verdict is reached, XTC_E_INVAL on NULL group,
 * XTC_E_AGAIN if another discard of this group is already in flight
 * (the caller may retry, or just wait for that one's verdict).
 * Callable from a fiber or a plain thread (the wait yields on a fiber,
 * sleeps the OS thread off one), but NOT from a member of the group
 * itself (that would be suicide mid-wait); XTC_E_INVAL if the caller is
 * a member. */
XTC_API int     xtc_arena_group_discard(xtc_arena_group_t *g, int reason,
                                        int64_t timeout_ns, int *out_all_gone);

/* Current member count (live + not-yet-reaped).  0 on NULL. */
XTC_API int     xtc_arena_group_size(xtc_arena_group_t *g);

#endif /* XTC_MCTX_H */
