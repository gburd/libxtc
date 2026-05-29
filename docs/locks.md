# Locks and synchronization in libxtc

This reference documents every locking and synchronization
primitive xtc provides, organized by the layer that hosts it.
The audience is a programmer choosing a primitive for a particular
use, or porting code from another system into the xtc
framework.

The presentation moves bottom-up.  Lower-layer primitives are
simpler, more constrained, and faster.  Higher-layer primitives
build on those below them and offer richer semantics, sometimes
at the cost of additional state or runtime overhead.  Choose the
lowest layer that meets the requirement.

The treatment of the lock manager (`xtc_lockmgr`) in section 4 is
deliberately patterned on the Berkeley DB locking subsystem
documentation, which is concise and accurate.  The deadlock
detection material in particular follows the BDB approach
because the algorithm is the BDB algorithm.

## Contents

  1. [Layer L0: `__os_*` system primitives](#layer-l0-__os_-system-primitives)
  2. [Layer L3 sync: `xtc_sync` and friends](#layer-l3-sync-xtc_sync-and-friends)
  3. [Layer L3 reader-writer: `xtc_lwlock`, `xtc_lrlock`, `xtc_rcu`](#layer-l3-reader-writer-xtc_lwlock-xtc_lrlock-xtc_rcu)
  4. [Layer L3 lock manager: `xtc_lockmgr`](#layer-l3-lock-manager-xtc_lockmgr)
  5. [Choosing a primitive](#choosing-a-primitive)
  6. [Locking and the coroutine model](#locking-and-the-coroutine-model)

---

## Layer L0: `__os_*` system primitives

Layer L0 wraps the operating system's native synchronization
primitives.  These are the fastest synchronization mechanisms
xtc offers because they translate directly to the kernel's
implementation: a `__os_mutex_lock` is one `pthread_mutex_lock`
call, no scheduler indirection, no fairness queueing, no
deadlock checks.  They are also the most dangerous because they
have no awareness of xtc tasks; a thread blocked on a
`__os_mutex` is blocked at the OS level, and the scheduler
cannot wake other coroutines on the same thread.

### `__os_mutex_t`

Plain mutual-exclusion lock.  On POSIX, a `pthread_mutex_t`; on
Windows, a `CRITICAL_SECTION`.

    int  __os_mutex_init   (__os_mutex_t *mu);
    int  __os_mutex_lock   (__os_mutex_t *mu);
    int  __os_mutex_unlock (__os_mutex_t *mu);
    int  __os_mutex_destroy(__os_mutex_t *mu);

The mutex is non-recursive; locking it from a thread that
already holds it is undefined.  The mutex is not interruptible;
there is no try-lock with a deadline at this layer.

The xtc convention is that calls into L0 mutex routines must
not run inside a coroutine that intends to yield while the lock
is held.  Holding `__os_mutex_t` across an `xtc_yield()` is a
priority inversion in the making: the scheduler cannot dispatch
other tasks on the thread, and any coroutine that blocks on the
same mutex from a different thread blocks that thread entirely.

Use cases:

  * Protecting a small data structure between OS threads when
    the critical section runs to completion without yielding.
  * Inside the implementation of a higher-layer primitive
    (`xtc_lwlock`, `xtc_lockmgr`, the slab allocator) where the
    critical section is bounded and known to be non-yielding.
  * Protecting state shared between an xtc thread and a
    non-xtc thread (for example, a callback from a third-party
    library running outside the xtc loop).

When a critical section is bounded by a few cache-line memory
accesses, `__os_mutex_t` is the right answer.  When the
critical section may yield, raise to `xtc_amutex` (section 2).
When readers far outnumber writers, raise to `xtc_lwlock` or
`xtc_lrlock` (section 3).  When the program acquires multiple
locks in arbitrary order under user control, use `xtc_lockmgr`
(section 4).

### `__os_rwlock_t`

Reader-writer lock.  On POSIX, a `pthread_rwlock_t`; on Windows,
an SRWLock.

    int  __os_rwlock_init        (__os_rwlock_t *rw);
    int  __os_rwlock_rdlock      (__os_rwlock_t *rw);
    int  __os_rwlock_wrlock      (__os_rwlock_t *rw);
    int  __os_rwlock_unlock      (__os_rwlock_t *rw);
    int  __os_rwlock_destroy     (__os_rwlock_t *rw);

The rules are the same as for `__os_mutex_t` regarding yielding.
The fairness policy is the platform default; xtc does not
enforce a writer-preference policy at this layer.

`__os_rwlock_t` exists for completeness and for code that ports
from a system that uses pthread rwlocks.  In new xtc code it is
almost always the wrong choice.  An `xtc_lwlock` (section 3) is
faster on the read side under contention; an `xtc_lrlock`
(section 3) admits wait-free reads where the data structure
permits it.

### `__os_cond_t`

Condition variable.  `pthread_cond_t` on POSIX; condition
variable on Windows.

    int  __os_cond_init    (__os_cond_t *c);
    int  __os_cond_wait    (__os_cond_t *c, __os_mutex_t *mu);
    int  __os_cond_timedwait(__os_cond_t *c, __os_mutex_t *mu,
                             int64_t deadline_ns);
    int  __os_cond_signal  (__os_cond_t *c);
    int  __os_cond_broadcast(__os_cond_t *c);
    int  __os_cond_destroy (__os_cond_t *c);

Standard condition-variable semantics.  The mutex must be held
when calling wait; it is released atomically with sleep and
re-acquired before wait returns.  Spurious wake-ups are
permitted; the caller must re-test the predicate.

The same caveats apply as for `__os_mutex_t`: a thread blocked
in `__os_cond_wait` is blocked at the OS level; an xtc loop on
that thread cannot make progress.  Use `xtc_notify`
(section 2) when the wait is part of a coroutine.

### `__os_sem_t`

Counting semaphore.  POSIX `sem_t` where available, falling back
to a counter behind a mutex+cond pair on platforms without
unnamed semaphore support.

    int  __os_sem_init    (__os_sem_t *s, unsigned initial);
    int  __os_sem_post    (__os_sem_t *s, unsigned n);
    int  __os_sem_wait    (__os_sem_t *s, unsigned n, int64_t deadline_ns);
    int  __os_sem_destroy (__os_sem_t *s);

`__os_sem_t` is mostly used as scaffolding.  Higher layers use
`xtc_sem` (section 2) which presents the same API but cooperates
with the coroutine scheduler.

---

## Layer L3 sync: `xtc_sync` and friends

These primitives sit on the L3 (`ptc/`) layer and are aware of
the xtc loop.  When a coroutine waits on one of them, the loop
parks the coroutine and dispatches another runnable task on the
same thread.  The wake path resumes the parked coroutine through
the loop's normal scheduling.

A coroutine may hold any of these locks across an `xtc_yield()`,
provided the calling code understands that other coroutines on
the same loop may queue waiting for the lock and that the
critical section therefore is not strictly bounded by wall time.

### `xtc_amutex_t`  --  async mutex

A mutex that parks the caller on the loop instead of blocking
the OS thread.

    int  xtc_amutex_create   (xtc_amutex_t **out);
    void xtc_amutex_destroy  (xtc_amutex_t *m);
    int  xtc_amutex_lock     (xtc_amutex_t *m, int64_t deadline_ns);
    int  xtc_amutex_try_lock (xtc_amutex_t *m);
    int  xtc_amutex_unlock   (xtc_amutex_t *m);

The fast path is one `compare_exchange` against the owner field.
Under contention, the caller registers a waker on the lock's
queue and yields; the unlocking coroutine wakes the head of the
queue.

Use `xtc_amutex_t` when the critical section may yield, perform
async I/O, or take a long time.  The cost relative to
`__os_mutex_t` is one extra atomic and a queue-link write on the
slow path; both primitives are within a small constant factor on
the fast path.  The cost relative to `xtc_lwlock` (section 3) is
that there is no separation between readers and writers.

### `xtc_notify_t`  --  one-shot signalling

A coroutine-aware condition variable.  The notify object holds
a single sticky signal-edge: `xtc_notify_signal()` makes any
subsequent or in-flight `xtc_notify_wait()` return; once
consumed, the edge is cleared.

    int  xtc_notify_create  (xtc_notify_t **out);
    void xtc_notify_destroy (xtc_notify_t *n);
    int  xtc_notify_signal  (xtc_notify_t *n);
    int  xtc_notify_wait    (xtc_notify_t *n, int64_t deadline_ns);

`xtc_notify_t` is the building block for higher-layer drain-wait
patterns.  `xtc_lrlock` uses it to let a writer wait for the
last reader on the old buffer to finish.

### `xtc_sem_t`  --  counting semaphore

A counting semaphore that parks waiters on the loop.

    int  xtc_sem_create     (unsigned initial, xtc_sem_t **out);
    void xtc_sem_destroy    (xtc_sem_t *s);
    int  xtc_sem_post       (xtc_sem_t *s, unsigned n);
    int  xtc_sem_acquire    (xtc_sem_t *s, unsigned n, int64_t deadline_ns);
    int  xtc_sem_try_acquire(xtc_sem_t *s, unsigned n);
    int  xtc_sem_count      (const xtc_sem_t *s);

Use `xtc_sem_t` for resource pools (database connections,
buffer slots) and for bounded-concurrency guards where the bound
is greater than one.  When the bound is one, an `xtc_amutex_t`
is faster.

### `xtc_abort_source_t`  --  cancellation

Not strictly a lock, but listed here because it is the
cooperative-cancellation primitive that other locks honour
through their deadline arguments.

    int  xtc_abort_source_create  (xtc_abort_source_t **out);
    void xtc_abort_source_destroy (xtc_abort_source_t *s);
    int  xtc_abort_source_fire    (xtc_abort_source_t *s, int reason);
    int  xtc_abort_source_token   (xtc_abort_source_t *s, xtc_abort_token_t *t);

Fire an abort source and every wait on a token derived from it
returns `XTC_E_ABORTED`.  The intent is to cancel a tree of
pending operations -- a request whose client disconnected, an
RPC whose deadline expired -- without leaking blocked
coroutines.

---

## Layer L3 reader-writer: `xtc_lwlock`, `xtc_lrlock`, `xtc_rcu`

These three primitives implement different points on the
read-bias / write-cost / consistency-model trade-off.

### `xtc_lwlock_t`  --  lightweight reader-writer lock

A reader-writer lock with shared and exclusive modes, modelled
directly on the PostgreSQL LWLock with the same encoding tricks
(HAS_WAITERS, WAKE_IN_PROGRESS, QUEUE_LOCKED in the high-order
bits of the state word).  The fast paths for shared-acquire and
shared-release each compile to a single atomic increment when
no exclusive waiter is present; the exclusive paths add a
compare-exchange.

    int  xtc_lwlock_init        (xtc_lwlock_t *lw);
    int  xtc_lwlock_acquire     (xtc_lwlock_t *lw, xtc_lwlock_mode_t mode);
    int  xtc_lwlock_release     (xtc_lwlock_t *lw);
    int  xtc_lwlock_destroy     (xtc_lwlock_t *lw);

Modes are `XTC_LWLOCK_SHARED` and `XTC_LWLOCK_EXCLUSIVE`.

Use `xtc_lwlock_t` when reads dominate and the protected region
is short enough to leave a non-yielding critical section.  The
contention behaviour matches PG's LWLock: shared acquirers see
fully concurrent fast paths until an exclusive waiter arrives,
at which point all subsequent acquirers queue.

`xtc_lwlock_t` is the right choice for ~95% of "this is a
hot read-mostly map" cases.

### `xtc_lrlock_t`  --  left-right (wait-free reads)

A two-buffer lock that gives readers wait-free access at the
cost of doubled storage.  Writers update the off-line buffer,
then publish (atomic index swap) and wait for readers on the
old buffer to finish; the reader-side cost is one atomic load.

    int          xtc_lrlock_create_ex     (const xtc_lrlock_opts_t *opts,
                                           xtc_lrlock_t **out);
    void         xtc_lrlock_destroy       (xtc_lrlock_t *lr);
    const void  *xtc_lrlock_read_data     (xtc_lrlock_t *lr);
    void        *xtc_lrlock_write_data    (xtc_lrlock_t *lr);
    void         xtc_lrlock_publish_full_sync(xtc_lrlock_t *lr);

Use `xtc_lrlock_t` when reads are absolutely on the hot path,
the data fits naturally in two copies (or the optional COW mode
that `xtc_lrlock` supports for sparse updates), and writes are
infrequent compared to reads.  Typical examples are configuration
tables, schema caches, and routing tables.

`xtc_lrlock_t` admits writes from concurrent threads only via
its single writer mutex; it is not lock-free for writers.

### `xtc_rcu_t`  --  read-copy-update with epoch reclamation

Wait-free reads, copy-on-write for updates, deferred reclamation
through epochs.

    int  xtc_rcu_create     (xtc_rcu_t **out);
    void xtc_rcu_destroy    (xtc_rcu_t *r);
    void xtc_rcu_read_lock  (xtc_rcu_t *r);
    void xtc_rcu_read_unlock(xtc_rcu_t *r);
    int  xtc_rcu_assign_pointer(xtc_rcu_t *r, void *new_ptr);
    void xtc_rcu_synchronize(xtc_rcu_t *r);
    void xtc_rcu_call       (xtc_rcu_t *r, void (*cb)(void *), void *arg);

Use `xtc_rcu_t` for graph data structures whose updates produce
new versions and whose old versions can be freed once no reader
still holds a pointer into them: relcache-style maps, version
trees, immutable index nodes.

The trade-off relative to `xtc_lrlock_t` is that RCU has no
internal copy management (the user produces and assigns the new
version) but admits multiple writer threads producing updates
concurrently with readers.

### When NOT to use any of these

  * When the workload is balanced (reads ~ writes), a plain
    `xtc_amutex_t` outperforms all three of these.
  * When acquisition order is dynamic and could induce a cycle,
    the lock-manager (section 4) is the only deadlock-safe
    answer.
  * Inside a coroutine that may yield while holding the lock,
    `xtc_lwlock` and `xtc_lrlock` are correct but the writer
    queue can grow unbounded; consider `xtc_amutex` or the
    lock-manager.

---

## Layer L3 lock manager: `xtc_lockmgr`

The xtc lock manager is the most general of the locking
primitives.  It admits arbitrary lock-acquisition orders, detects
and resolves deadlocks, supports timeouts and cancellation, and
manages large numbers of fine-grained locks identified by 64-bit
keys.  It is also the most expensive: each lock acquire takes one
or two cache-line writes plus a hash lookup, plus the cost of
honouring the deadlock detector's victim selection policy.

The lock manager is a port of the Berkeley DB locking subsystem
to the xtc framework.  It uses the same conflict matrix
parameterization, the same notion of lockers and lock objects,
and the same deadlock-detection algorithm (waits-for-graph
search).  Code that ported between BDB and Cassandra by way of
DataStax's lock manager will recognize the surface immediately.

### Conceptual model

A *locker* is an entity that holds locks.  In xtc this is most
often an `xtc_proc` -- one entity per coroutine -- but a locker
can be any 64-bit handle the application chooses to allocate.

A *lock object* is what the locker holds the lock on.  Lock
objects are identified by a 64-bit key and an optional
namespace tag, both selected by the application.  In a database
context, the key might be a (database-id, table-id, page-id,
row-id) tuple folded down to 64 bits; in a higher-level
application, the key might be the address of the protected
structure.

A *lock mode* describes the access being requested.  The
default conflict matrix is the BDB nine-mode matrix (NL, IS, IX,
S, SIX, U, X, R, W); applications can install a custom matrix
through `xtc_lockmgr_create_ex`.

### API

    int   xtc_lockmgr_create     (xtc_lockmgr_t **out);
    int   xtc_lockmgr_create_ex  (const xtc_lockmgr_opts_t *opts,
                                  xtc_lockmgr_t **out);
    void  xtc_lockmgr_destroy    (xtc_lockmgr_t *lm);

    int   xtc_lockmgr_locker_alloc(xtc_lockmgr_t *lm,
                                   xtc_locker_t *out);
    int   xtc_lockmgr_locker_free (xtc_lockmgr_t *lm,
                                   xtc_locker_t  l);

    int   xtc_lockmgr_acquire    (xtc_lockmgr_t *lm,
                                  xtc_locker_t  l,
                                  uint64_t      object_key,
                                  xtc_lock_mode_t mode,
                                  int64_t       deadline_ns,
                                  xtc_lock_t   *out);
    int   xtc_lockmgr_release    (xtc_lockmgr_t *lm, xtc_lock_t lk);
    int   xtc_lockmgr_release_all(xtc_lockmgr_t *lm, xtc_locker_t l);

    int   xtc_lockmgr_stats      (xtc_lockmgr_t *lm,
                                  xtc_lockmgr_stats_t *out);

### Deadlock detection

A deadlock is a cycle in the waits-for graph: a set of lockers
each waiting on a lock held by another locker in the set, with
the cycle closing back on the original locker.  Without
intervention, a deadlock is permanent: each locker waits for the
next, and none can release anything because none can proceed.

The lock manager runs a periodic deadlock detector that walks
the waits-for graph looking for cycles.  When it finds one, it
breaks the cycle by aborting one of the lockers in it -- the
victim.  The victim's `xtc_lockmgr_acquire` returns
`XTC_E_DEADLK`; the application is responsible for reacting,
typically by releasing every lock the locker holds and retrying
the operation that triggered the cycle.

The detector runs in its own thread.  Detection frequency is
configurable through `xtc_lockmgr_opts_t.detect_interval_ns`;
the default is 100 ms.  Setting the interval to zero disables
the detector and reduces the lock manager to a deadlock-prone
fast path; this is appropriate only when the application
guarantees a global lock ordering and uses the lock manager for
its other features (timeouts, statistics, fine-grained
acquisition).

#### Victim selection

Once the detector finds a cycle, it must choose which locker in
the cycle to abort.  The application chooses the policy through
`xtc_lockmgr_opts_t.victim`:

    XTC_LOCK_VICTIM_RANDOM    -- pick uniformly from the cycle
                                 (the BDB default; called
                                 XTC_LOCK_VICTIM_DEFAULT for
                                 historical compatibility)
    XTC_LOCK_VICTIM_OLDEST    -- pick the locker with the
                                 longest existence
    XTC_LOCK_VICTIM_YOUNGEST  -- pick the most recently created
                                 locker
    XTC_LOCK_VICTIM_MIN_LOCKS -- pick the locker with the
                                 fewest locks held; minimizes
                                 the rollback cost
    XTC_LOCK_VICTIM_MAX_LOCKS -- pick the locker with the most
                                 locks; aborts the largest
                                 transaction
    XTC_LOCK_VICTIM_MIN_WRITE -- pick the locker holding the
                                 fewest write locks
    XTC_LOCK_VICTIM_MAX_WRITE -- pick the locker holding the
                                 most write locks
    XTC_LOCK_VICTIM_EXPIRE    -- pick only lockers whose own
                                 deadlines have already passed;
                                 if no expired locker exists,
                                 abort no one and let the
                                 detector try again at the next
                                 tick
    XTC_LOCK_VICTIM_CUSTOM    -- the application supplies the
                                 selection function in
                                 opts.victim_pick_fn

The choice of policy depends on the workload.  RANDOM avoids
starvation and is a defensible default.  YOUNGEST tends to abort
short transactions and let long ones complete, reducing total
work lost; this is the wound-wait style.  OLDEST is the wait-die
style and is biased the other way.  MIN_LOCKS is a useful policy
when the application can cheaply reproduce the work of a small
transaction.

#### Timeouts

Independently of the deadlock detector, an acquisition can fail
because its own deadline has expired.  The `deadline_ns`
argument to `xtc_lockmgr_acquire` is an absolute deadline in
`xtc_now_ns()` units.  When the deadline elapses without the
lock being granted, the call returns `XTC_E_TIMEDOUT`.

Timeouts are independent of deadlock detection but interact
with it.  The `XTC_LOCK_VICTIM_EXPIRE` policy makes the
detector wait for natural timeouts before resorting to abort:
if a deadlock cycle exists but no locker has its own deadline
yet, the detector takes no action; once a locker's deadline
passes, it becomes the natural victim and the cycle resolves.
This policy is appropriate when timeouts are short relative to
the detection interval and when application code already has
robust deadline handling.

#### Performance and tuning

The detector walks the waits-for graph each tick.  The walk is
linear in the number of waiting lockers; idle lockers (those
not currently waiting) cost nothing.  In a workload with many
idle lockers and few contention points the detector's overhead
is negligible.  In a workload with many contention points the
overhead can be observed; tune `detect_interval_ns` upward to
amortize.

`xtc_lockmgr_stats_t` exposes:

    n_acquires           -- successful acquisitions, lifetime
    n_releases           -- releases, lifetime
    n_waits              -- acquisitions that had to wait
    n_timeouts           -- acquisitions that returned TIMEDOUT
    n_deadlocks_found    -- cycles found by the detector
    n_aborts             -- victim aborts performed
    detect_iterations    -- detector ticks elapsed
    waits_for_edges      -- size of the current waits-for graph

### When to use the lock manager

Use `xtc_lockmgr_t` when:

  * The set of lockable objects is dynamic and large.  An
    application protecting individual rows of a million-row
    table cannot reasonably allocate a `xtc_amutex_t` per row;
    the lock manager indexes by 64-bit key and folds collisions
    onto a single hash bucket worth of state.
  * The acquisition order is dictated by the user (a SQL
    query plan, a graph traversal where edges are followed in
    response order) and a global ordering cannot be enforced.
    Deadlock detection is the only correct response to such
    workloads.
  * Multiple lock modes participate: shared, exclusive,
    intention-shared, intention-exclusive, update.  The
    full BDB conflict matrix is supported out of the box.
  * Statistics and observability matter.  The lock manager
    tracks contention, wait queues, and conflict counts at low
    cost.

Do not use the lock manager when:

  * The number of locks is small and known statically; an
    array of `xtc_amutex_t` is faster per-acquisition.
  * The workload reads more than it writes; one of the
    reader-writer primitives in section 3 is faster.
  * The critical section is so short that the hash-table
    lookup itself dominates the cost; an `__os_mutex_t` is
    cheaper.

---

## Choosing a primitive

The decision tree below covers the common cases.  Read it as a
flowchart; each leaf points to the recommended primitive.

  Q: Does the critical section yield, await, or block on async
     I/O?
     -> Yes -> Q: Are multiple lock objects involved with
                  user-driven ordering?
                  -> Yes -> xtc_lockmgr
                  -> No  -> Q: Is the workload read-heavy?
                                -> Yes -> xtc_lwlock or xtc_lrlock
                                -> No  -> xtc_amutex
     -> No  -> Q: Are multiple lock objects involved with
                  user-driven ordering?
                  -> Yes -> xtc_lockmgr
                  -> No  -> __os_mutex_t

For signalling rather than mutual exclusion:

  one-shot edge      -> xtc_notify
  counting           -> xtc_sem
  cancellation tree  -> xtc_abort_source

For the special case of "pointer that updates rarely, readers
must not block ever":

  fits-in-two-buffers   -> xtc_lrlock
  graph with reclamation -> xtc_rcu

---

## Locking and the coroutine model

xtc coroutines are cooperative: a coroutine continues to run on
its thread until it yields, awaits, or returns.  The locking
implications of this design:

  * Two coroutines on the same loop never contend on a lock at
    the same instant; they are serialized by the loop.  An
    `xtc_amutex_t` between them is essentially a flag check.

  * Two coroutines on different loops contend through the L3
    primitive's normal mechanism; the difference is that the
    waiting coroutine yields to its own loop instead of
    blocking the thread.

  * Holding any lock across an `xtc_yield()` is permitted but
    must be reasoned about.  The yield gives other coroutines
    on the same loop the chance to run; if any of them tries
    to acquire the same lock, it joins the queue.  Long-held
    locks effectively serialize the loop.

  * Holding an `__os_mutex_t` (L0) across an `xtc_yield()` is a
    correctness bug: the loop cannot dispatch coroutines that
    don't need the mutex either, because the OS-level lock
    blocks the thread.  Use `xtc_amutex_t` instead.

  * Lock manager waits respect cancellation.  An
    `xtc_lockmgr_acquire` whose caller's abort token is fired
    returns `XTC_E_ABORTED` promptly; the locker is removed
    from the wait graph and the deadlock detector no longer
    sees its waits-for edges.

---

## See also

  * [`xtc_lwlock(3)`](../man/man3/xtc_lwlock.3) -- LWLock manual page.
  * [`xtc_lrlock(3)`](../man/man3/xtc_lrlock.3) -- LRLock manual page.
  * [`xtc_lockmgr(3)`](../man/man3/xtc_lockmgr.3) -- Lock manager manual page.
  * [`xtc_sync(3)`](../man/man3/xtc_sync.3) -- Sync primitives manual page.
  * [`xtc_rcu(3)`](../man/man3/xtc_rcu.3) -- RCU manual page.
  * `docs/M_LRLOCK_COW.md` -- design notes on lrlock COW mode.
  * `docs/M_BEAM_LESSONS.md` -- discussion of supervisor / locker
    interaction lessons drawn from BEAM.

The Berkeley DB locking subsystem documentation, available in
the BDB source tree, is the canonical reference for the
deadlock-detection model adopted here.
