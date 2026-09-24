/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_lockmgr.h
 *	Heavyweight lock manager: per-object queue locks with
 *	deadlock detection and a 9-mode intent (RIW) lattice.
 *
 *	The conflict matrix is configurable at create-time so
 *	callers can supply their own (e.g. a 5-mode subset, a 5-mode
 *	concurrent-data-store set, or an 8-mode set).
 *
 *	Default 9x9 conflict matrix (1 = conflict, 0 = compatible),
 *	verbatim from libdb's `db_riw_conflicts`:
 *
 *	         NL   S   X   WT  IX  IS  IWR RU  WW
 *	    NL    0   0   0   0   0   0   0   0   0
 *	    S     0   0   1   0   1   0   1   0   1
 *	    X     0   1   1   1   1   1   1   1   1
 *	    WT    0   0   0   0   0   0   0   0   0   (waiter placeholder)
 *	    IX    0   1   1   0   0   0   0   1   1
 *	    IS    0   0   1   0   0   0   0   0   1
 *	    IWR   0   1   1   0   0   0   0   1   1
 *	    RU    0   0   1   0   1   0   1   0   0   (read-uncommitted)
 *	    WW    0   1   1   0   1   1   1   0   1   (was-written)
 *
 *	Mode encyclopedia:
 *	  NL  -- no lock granted (placeholder)
 *	  S   -- shared / read
 *	  X   -- exclusive / write
 *	  WT  -- wait placeholder; not granted, doesn't block
 *	  IX  -- intent exclusive (will lock children with X)
 *	  IS  -- intent shared
 *	  IWR -- intent read+write (combo, used by index access)
 *	  RU  -- read-uncommitted (degree-1 isolation; sees dirty writes)
 *	  WW  -- was-written (released X but txn not committed; blocks
 *	        new readers but not other ex-writers in the same view)
 *
 *	Deadlock detection runs periodically by default; callers can
 *	also force a check or configure on-every-conflict mode.  Six
 *	victim-selection policies are configurable.
 */

#ifndef XTC_LOCKMGR_H
#define XTC_LOCKMGR_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_lockmgr xtc_lockmgr_t;
typedef uint64_t           xtc_locker_t;     /* opaque locker ID */

typedef enum xtc_lock_mode {
	XTC_LOCK_NL   = 0,   /* not granted / no lock */
	XTC_LOCK_S    = 1,   /* shared / read */
	XTC_LOCK_X    = 2,   /* exclusive / write */
	XTC_LOCK_WAIT = 3,   /* wait placeholder */
	XTC_LOCK_IX   = 4,   /* intent exclusive */
	XTC_LOCK_IS   = 5,   /* intent shared */
	XTC_LOCK_IWR  = 6,   /* intent read+write */
	XTC_LOCK_RU   = 7,   /* read-uncommitted / degree-1 */
	XTC_LOCK_WW   = 8,   /* was-written downgrade state */
	XTC_LOCK_NMODES = 9
} xtc_lock_mode_t;

typedef enum xtc_lock_victim_policy {
	XTC_LOCK_VICTIM_DEFAULT   = 0,    /* alias for RANDOM (the default) */
	XTC_LOCK_VICTIM_RANDOM    = 1,
	XTC_LOCK_VICTIM_OLDEST    = 2,
	XTC_LOCK_VICTIM_YOUNGEST  = 3,
	XTC_LOCK_VICTIM_MIN_LOCKS = 4,
	XTC_LOCK_VICTIM_MAX_LOCKS = 5,
	XTC_LOCK_VICTIM_MIN_WRITE = 6,
	XTC_LOCK_VICTIM_MAX_WRITE = 7,
	XTC_LOCK_VICTIM_EXPIRE    = 8,    /* only victims with expired timeouts */
	XTC_LOCK_VICTIM_CUSTOM    = 9     /* opts.victim_pick_fn supplies the choice */
} xtc_lock_victim_policy_t;

/* Custom victim-picker.  Called when victim policy is
 * XTC_LOCK_VICTIM_CUSTOM and the deadlock detector finds a cycle.
 * The implementation receives the locker IDs participating in the
 * cycle (length n_candidates >= 2) and must return an index in
 * [0, n_candidates).  May call into xtc_proc / xtc_send / xtc_recv
 * during the choice -- e.g. to consult an external decision oracle
 * or a randomness producer.  Must not block indefinitely. */
typedef int (*xtc_lock_victim_pick_fn)(const uint64_t *candidate_lockers,
                                       int n_candidates,
                                       void *user);

typedef enum xtc_lock_detect_mode {
	XTC_LOCK_DETECT_PERIODIC  = 0,    /* background thread runs every period_ns */
	XTC_LOCK_DETECT_ON_BLOCK  = 1,    /* every conflict triggers detector synchronously */
	XTC_LOCK_DETECT_NONE      = 2     /* no detection; caller must drive */
} xtc_lock_detect_mode_t;

typedef struct xtc_lockmgr_opts {
	int      n_partitions;            /* hash buckets; 0 = default 64 */
	int64_t  detect_interval_ns;      /* default 100 ms (PERIODIC mode) */
	xtc_lock_victim_policy_t victim;
	xtc_lock_detect_mode_t   detect_mode;

	/* Custom victim picker.  Used only when victim ==
	 * XTC_LOCK_VICTIM_CUSTOM.  May be NULL otherwise. */
	xtc_lock_victim_pick_fn  victim_pick_fn;
	void                    *victim_pick_user;

	/* Optional custom conflict matrix.  If NULL, the 9-mode RIW
	 * default above is used.  If non-NULL, must be n_modes*n_modes
	 * bytes (row-major, [held*n_modes + requested]). */
	const uint8_t *conflicts;
	int            n_modes;           /* 0 = default 9 */
} xtc_lockmgr_opts_t;

#define XTC_LOCKMGR_OPTS_DEFAULT { \
	.n_partitions       = 64, \
	.detect_interval_ns = 100LL * 1000 * 1000, \
	.victim             = XTC_LOCK_VICTIM_DEFAULT, \
	.detect_mode        = XTC_LOCK_DETECT_PERIODIC, \
	.victim_pick_fn     = NULL, \
	.victim_pick_user   = NULL, \
	.conflicts          = NULL, \
	.n_modes            = 0 \
}

/* Lock-vec compound op kinds. */
typedef enum xtc_lock_op {
	XTC_LOCK_OP_GET     = 0,
	XTC_LOCK_OP_PUT     = 1,
	XTC_LOCK_OP_PUT_ALL = 2,    /* release_all for the locker; obj ignored */
	XTC_LOCK_OP_UPGRADE = 3,    /* atomic mode upgrade (caller must already hold) */
	XTC_LOCK_OP_DOWNGRADE = 4
} xtc_lock_op_t;

typedef struct xtc_lock_req {
	xtc_lock_op_t   op;
	xtc_lock_mode_t mode;
	const void     *obj;
	size_t          obj_size;
	int64_t         timeout_ns;       /* ignored for non-GET ops */
} xtc_lock_req_t;

typedef struct xtc_lockmgr_stat {
	int       n_held;             /* total granted entries */
	int       n_waiting;          /* total waiters */
	int       n_objects;          /* live lock objects */
	int       n_lockers;          /* allocated locker IDs */
	uint64_t  n_acquires;         /* lifetime acquire count */
	uint64_t  n_releases;
	uint64_t  n_deadlocks_found;  /* victim-aborts performed */
	uint64_t  n_timeouts;         /* timed-out waits */
} xtc_lockmgr_stat_t;

/*
 * PUBLIC: int  xtc_lockmgr_create __P((const xtc_lockmgr_opts_t *, xtc_lockmgr_t **));
 * PUBLIC: void xtc_lockmgr_destroy __P((xtc_lockmgr_t *));
 *
 * PUBLIC: int  xtc_lockmgr_id __P((xtc_lockmgr_t *, xtc_locker_t *));
 * PUBLIC: int  xtc_lockmgr_id_free __P((xtc_lockmgr_t *, xtc_locker_t));
 * PUBLIC: int  xtc_lockmgr_id_set_timeout __P((xtc_lockmgr_t *, xtc_locker_t, int64_t));
 *
 * PUBLIC: int  xtc_lock_get __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t, xtc_lock_mode_t, int64_t));
 * PUBLIC: int  xtc_lock_put __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t));
 * PUBLIC: int  xtc_lock_release_all __P((xtc_lockmgr_t *, xtc_locker_t));
 * PUBLIC: int  xtc_lock_upgrade __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t, xtc_lock_mode_t));
 * PUBLIC: int  xtc_lock_downgrade __P((xtc_lockmgr_t *, xtc_locker_t, const void *, size_t, xtc_lock_mode_t));
 * PUBLIC: int  xtc_lock_vec __P((xtc_lockmgr_t *, xtc_locker_t, xtc_lock_req_t *, int, int *));
 *
 * PUBLIC: int  xtc_lockmgr_check_deadlocks __P((xtc_lockmgr_t *, int *));
 * PUBLIC: int  xtc_lockmgr_failchk __P((xtc_lockmgr_t *, xtc_locker_t));
 * PUBLIC: int  xtc_lockmgr_stat __P((const xtc_lockmgr_t *, xtc_lockmgr_stat_t *));
 * PUBLIC: int  xtc_lockmgr_n_held __P((const xtc_lockmgr_t *));
 * PUBLIC: int  xtc_lockmgr_n_waiting __P((const xtc_lockmgr_t *));
 */

XTC_API int  xtc_lockmgr_create(const xtc_lockmgr_opts_t *opts, xtc_lockmgr_t **out);
XTC_API void xtc_lockmgr_destroy(xtc_lockmgr_t *mgr);

XTC_API int  xtc_lockmgr_id(xtc_lockmgr_t *mgr, xtc_locker_t *out);
XTC_API int  xtc_lockmgr_id_free(xtc_lockmgr_t *mgr, xtc_locker_t l);

/* Set a deadline for this locker.  When DETECT_PERIODIC fires, lockers
 * past their timeout are considered "expired" and get higher victim
 * priority under VICTIM_EXPIRE policy.  timeout_ns < 0 = no expiry. */
XTC_API int  xtc_lockmgr_id_set_timeout(xtc_lockmgr_t *mgr, xtc_locker_t l,
                                        int64_t timeout_ns);

/* Single acquire.  timeout_ns: -1 = forever, 0 = NOWAIT.  Returns
 * XTC_OK / XTC_E_AGAIN / XTC_E_DEADLK / XTC_E_INVAL / XTC_E_NOMEM.
 *
 * A locker that already holds a lock on obj and asks for another mode
 * (a CONVERSION) is checked against every OTHER holder, exactly like a
 * fresh request; a mode its held one already dominates is a no-op; a
 * dominating mode raises the held lock in place; any other mode is
 * granted as an additional lock if compatible.  A conversion is never
 * queued behind waiters (they may be waiting for this locker).  Before
 * 1.50 a conversion was checked only against the locker's own mode --
 * granting past a conflicting holder -- and could silently WEAKEN the
 * held lock (S re-requested as IS became IS). */
XTC_API int  xtc_lock_get(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                          const void *obj, size_t obj_size,
                          xtc_lock_mode_t mode, int64_t timeout_ns);

XTC_API int  xtc_lock_put(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                          const void *obj, size_t obj_size);

XTC_API int  xtc_lock_release_all(xtc_lockmgr_t *mgr, xtc_locker_t locker);

/* Atomic mode change on a held lock.  "Stronger" and "weaker" come from
 * the conflict MATRIX, not the enum's numeric order: new_mode must
 * dominate (upgrade) or be dominated by (downgrade) the held mode --
 * exclude at least / at most what it excludes -- else XTC_E_INVAL.
 * Upgrade waits if another holder conflicts.  The API has no timeout
 * argument, so the wait is bounded by the locker's deadline
 * (xtc_lockmgr_id_set_timeout): past it, XTC_E_AGAIN; with none it waits
 * until granted or chosen as a deadlock victim (XTC_E_DEADLK).  A
 * granted upgrade replaces the held lock (one xtc_lock_put releases it).
 * Downgrade is always non-blocking and promotes any waiters that the new
 * mode no longer conflicts with.  Before 1.50 both used enum order (so
 * IX -> IS was refused and IX -> S allowed), and upgrade ignored the
 * deadline and left the old lock held beside the new one. */
XTC_API int  xtc_lock_upgrade(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                              const void *obj, size_t obj_size,
                              xtc_lock_mode_t new_mode);
XTC_API int  xtc_lock_downgrade(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                                const void *obj, size_t obj_size,
                                xtc_lock_mode_t new_mode);

/* Compound request, ALL-OR-NONE for acquires.  Every op is validated
 * first (a bad op / NULL obj / zero obj_size / un-GETable mode returns
 * XTC_E_INVAL with nothing executed), then the ops run in order.
 *
 * On success returns XTC_OK and *out_executed == n_reqs.
 *
 * If an op fails (a GET that would block past its timeout_ns ->
 * XTC_E_AGAIN, deadlock victim -> XTC_E_DEADLK, XTC_E_NOMEM, or a
 * PUT/UPGRADE/DOWNGRADE that is rejected) the call ROLLS BACK: every
 * GET this call performed AFTER the last non-GET op is undone, newest
 * first -- a newly granted lock is released, a mode this call raised on
 * a lock the locker already held is lowered back -- and the failing
 * op's rc is returned.  So a vector of only GETs leaves NO partial
 * state and *out_executed == 0.
 *
 * PUT, PUT_ALL, UPGRADE and DOWNGRADE are NOT reversible (a released
 * lock may already have been granted to another locker), so each is a
 * rollback barrier: it and everything before it stay applied, and
 * *out_executed is the length of that retained prefix (the index of the
 * op after the last barrier that ran).  Put the releases/mode changes
 * FIRST and the GETs after them if you need the GETs to be atomic.
 * A deadlock victim has had ALL its locks released by the detector,
 * including ones held before the call; *out_executed still reports the
 * retained prefix, but nothing is held. */
XTC_API int  xtc_lock_vec(xtc_lockmgr_t *mgr, xtc_locker_t locker,
                          xtc_lock_req_t *reqs, int n_reqs, int *out_executed);

XTC_API int  xtc_lockmgr_check_deadlocks(xtc_lockmgr_t *mgr, int *n_aborted);

/* Mark a locker as "failed" (typically because the owning thread
 * died).  Releases all its locks and aborts any waits. */
XTC_API int  xtc_lockmgr_failchk(xtc_lockmgr_t *mgr, xtc_locker_t locker);

XTC_API int  xtc_lockmgr_stat(const xtc_lockmgr_t *mgr, xtc_lockmgr_stat_t *out);
XTC_API int  xtc_lockmgr_n_held(const xtc_lockmgr_t *mgr);
XTC_API int  xtc_lockmgr_n_waiting(const xtc_lockmgr_t *mgr);

#endif /* XTC_LOCKMGR_H */
