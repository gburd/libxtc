/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_dst_inject.h
 *	DST bug-injection harness (the "bug-detection latency" yardstick).
 *
 *	FoundationDB / TigerBeetle back the claim "deterministic simulation
 *	finds real bugs" by PLANTING a known bug and proving the simulator
 *	catches it within a small number of seeds, deterministically, with
 *	a replayable trace.  That -- not a code-coverage percentage -- is
 *	the metric that inspires confidence: "if you break a safety
 *	invariant, DST catches it fast and hands you the exact seed."
 *
 *	This header defines the injection points.  In a normal build
 *	XTC_DST_INJECT_BUG is undefined and every check compiles to 0, so
 *	the harness is completely absent from production and from the
 *	default test build.  A dedicated build defines
 *	-DXTC_DST_INJECT_BUG=<n> to activate exactly ONE planted bug, and
 *	the DST suite is then expected to FAIL (an invariant fires) within
 *	a bounded seed count -- driven by scripts/dst-bug-inject.sh, which
 *	asserts each planted bug is caught.  A planted bug the sweep does
 *	NOT catch is a hole in the DST coverage of that safety property.
 *
 *	Each bug id targets a DST-reachable safety-critical site whose
 *	violation a specific invariant checker or capstone test detects:
 *
 *	  1  LOSTWAKE   -- proc.c drops a mailbox waker fire.  A parked
 *	                   receiver never wakes; the sim's quiescence /
 *	                   lost-wakeup invariant (a proc still alive but no
 *	                   runnable work and no timer) fires -> XTC_E_DEADLK.
 *	  2  LOCKEXCL   -- lock_mgr.c grants a conflicting lock.  Mutual
 *	                   exclusion breaks; test_sim_compose's lock-held
 *	                   witness (count must stay <= 1) fires.
 *	  3  NODURABLE  -- wal.c skips the fdatasync but still acks the
 *	                   commit.  An acked commit is not durable; the
 *	                   test_sim_compose_crash durability invariant (every
 *	                   acked commit present after recovery) fires.
 *	  4  CREDITWIN  -- credit.c posts the free-credit semaphore twice on
 *	                   release.  The sliding window can then be exceeded;
 *	                   test_sim_credit's window invariant (in-flight
 *	                   never > window) fires.
 *	  5  RESLEAK    -- res.c drops the decrement in xtc_res_release.  The
 *	                   accountant never returns to zero; test_sim_res'
 *	                   conservation invariant (final used == 0) fires.
 *	  6  RESOVER    -- res.c skips the cap check in xtc_res_acquire.  The
 *	                   cap can be blown through; test_sim_res' SAFETY
 *	                   invariant (used never > cap) fires.
 *	  7  CHANDROP   -- chan.c drops a message in xtc_chan_mpmc_try_recv
 *	                   (advances tail without returning it).  An item is
 *	                   lost; test_sim_chan's mpmc exactly-once invariant
 *	                   (no drop/dup) fires.
 *	  8  REGDUP     -- reg.c lets a second pid register a name already
 *	                   held.  At-most-one-holder breaks; test_sim_reg's
 *	                   duplicate-registration invariant fires.
 *	  9  SAGAORDER  -- saga.c compensates the completed prefix FORWARD
 *	                   instead of in reverse.  The undo order is wrong;
 *	                   test_sim_saga's exact-reverse-order invariant
 *	                   fires.
 *
 *	 10  BATCHWAKE -- batch.c drops the submit notification; parked driver
 *	                   fails the logical progress bound.
 *	 11  BATCHCAP  -- batch.c admits beyond max_active; active cap fails.
 *	 12  BATCHRES  -- batch.c retains terminal reservations; conservation fails.
 *	 13  BATCHSTALE -- batch.c ignores handle identity; wrong request cancelled.
 *	 14  BATCHDONE -- batch.c retains DONE members in the next vector.
 *	 15  SCHEDFAIR -- sched.c ignores the mandatory fairness override;
 *	                   the bounded-service invariant fails.
 *	 16  BATCHDROP -- batch.c drops an mt hand-in drained into a full
 *	                   group; an accepted request never completes.
 *	 17  WALKILL   -- examples/06_sqlxtc/wal.c runs a commit without the
 *	                   cancellation mask, so a killed committer leaves a
 *	                   dead waiter (or the leadership) behind; the log
 *	                   wedges (test_sim_wal_kill: LIVENESS).
 *	 18  IDXSKIP   -- examples/06_sqlxtc/xstore.c's index rebuild at an
 *	                   open skips the last index, so after a crash
 *	                   recovery that index misses rows
 *	                   (test_sim_sqlxtc_reopen: INDEX).
 *	 19  EVICTNOWB -- examples/06_sqlxtc/bufmgr.c's evictor drops every
 *	                   4th dirty victim without writing it back; the
 *	                   page reloads stale (test_sim_sqlxtc_evict: a lost
 *	                   or wrong row).
 *	 20  OCKDROP   -- examples/06_sqlxtc/wal.c's online checkpoint drops
 *	                   the last frame of each catch-up / finish copy, so
 *	                   a commit acked during the checkpoint is missing
 *	                   from the new log (test_sim_sqlxtc_ckpt: a lost
 *	                   or wrong row after crash recovery).
 *	 21  PRUNEHZN  -- examples/06_sqlxtc/xstore.c's prune-on-write
 *	                   deletes the version AT the snapshot horizon (the
 *	                   one the oldest snapshot reads) when a newer one
 *	                   exists (test_sim_sqlxtc_gc: SNAPSHOT).
 *	 22  SHIFTLO   -- examples/06_sqlxtc/btree.c's cursor_seek skips the
 *	                   lower-fence re-check when a parked cursor resumes,
 *	                   so after a redistribution moved its position into
 *	                   the LEFT sibling it resumes at the page's first key
 *	                   and skips the keys that moved
 *	                   (test_sim_btree_scan: skipped anchor).
 *	 24..29        reserved (assigned on other 2.2 branches).
 *	 30  PIDEXEC   -- proc.c mints pids with the 2.1 executor-relative
 *	                   loop id (exec slot + 1, 0 standalone), so a send
 *	                   to a pid of another executor reaches the proc in
 *	                   the same slot of the sender's own
 *	                   (test_sim_pid_xexec: MISDELIVERED).
 *	 31  EXITNEG   -- proc.c encodes a kill and an exit code as reason+1
 *	                   so that 0 means none, as 2.1 did: a kill with -1
 *	                   is dropped and xtc_exit_self(-1) reads CLEAN
 *	                   (test_sim_down_kind: misclassified or missing).
 *	 32  FAULTKIND -- proc.c drops the contained-fault override, so a
 *	                   fault in a user recovery frame reads EXIT
 *	                   (test_sim_down_kind).
 *	 33  KILLKIND  -- proc.c reports a death by xtc_exit_pid as EXIT,
 *	                   not KILLED (test_sim_down_kind).
 *	 34  INITEXIT  -- svr.c returns normally after a failed init, so the
 *	                   server's DOWN reads CLEAN (test_sim_down_kind).
 *	 54  RCU_SLOT_GEN -- rcu.c skips the registry-generation check on a
 *	                   fiber's cached reader slot, so a fiber that read
 *	                   through xtc_rcu_fini publishes into a freed slot
 *	                   that synchronize does not scan (test_sim_rcu: a
 *	                   reader saw a freed node).
 *	 63  RCU_NOSLOT -- rcu.c does not count a read-side that got no
 *	                   reader slot, so synchronize frees under it
 *	                   (test_sim_rcu).
 *	 64  RCU_RETIRE_FREE -- rcu.c frees an unrecordable retirement at
 *	                   once, under its readers (test_sim_rcu).
 *	 65  LRLOCK_OPLOG_DROP -- lock_lr.c drops an op the oplog cannot
 *	                   grow to hold (test_sim_lrlock).
 *
 *	NOT plantable under DST (an honest gap, recorded so it is not
 *	mistaken for missing coverage): the xtc_amutex mutual-exclusion and
 *	lost-wakeup invariants.  test_sim_latch's critical section is
 *	deliberately yield-free, so the single-threaded cooperative
 *	scheduler runs each section atomically and a double-grant never
 *	interleaves a lost update; and a dropped hand-off wake is invisible
 *	because the sim reschedules a parked fiber from its state (as
 *	test_sim_wake_park documents), not from a wake fd, and unlock sets
 *	w->granted under the lock before waking.  Both amutex bugs were
 *	built and confirmed to pass the test both ways, so they are dropped
 *	rather than shipped as coverage theater.  Mutual exclusion IS proven
 *	by bug 2 (LOCKEXCL) against the heavyweight lock manager.
 *
 *	When you add a new safety invariant, add a planted-bug id here and a
 *	case to scripts/dst-bug-inject.sh so DST must prove it catches it.
 */

#ifndef XTC_DST_INJECT_H
#define XTC_DST_INJECT_H

/* XTC_DST_BUG(n) is 1 iff the build activated planted bug n (xtc_sim.h,
 * where an application's planted bugs get it too). */
#include "xtc_sim.h"

/* Symbolic ids (keep in sync with scripts/dst-bug-inject.sh).  The
 * sqlxtc engine's ids (3, 17-23, 72-73, 79-81) live in examples/06_sqlxtc/sx_dst.h. */
#define XTC_DST_BUG_LOSTWAKE   1   /* proc.c: drop a mailbox waker fire */
#define XTC_DST_BUG_LOCKEXCL   2   /* lock_mgr.c: grant a conflicting lock */
#define XTC_DST_BUG_CREDITWIN  4   /* credit.c: double-post the free-credit sem */
#define XTC_DST_BUG_RESLEAK    5   /* res.c: drop the release decrement */
#define XTC_DST_BUG_RESOVER    6   /* res.c: skip the acquire cap check */
#define XTC_DST_BUG_CHANDROP   7   /* chan.c: drop an mpmc message on recv */
#define XTC_DST_BUG_REGDUP     8   /* reg.c: allow a duplicate registration */
#define XTC_DST_BUG_SAGAORDER  9   /* saga.c: compensate forward, not reverse */

#define XTC_DST_BUG_BATCHWAKE  10  /* batch.c: omit submit notification */
#define XTC_DST_BUG_BATCHCAP   11  /* batch.c: ignore max_active admission cap */
#define XTC_DST_BUG_BATCHRES   12  /* batch.c: retain terminal reservation */
#define XTC_DST_BUG_BATCHSTALE 13  /* batch.c: ignore handle identity */
#define XTC_DST_BUG_BATCHDONE  14  /* batch.c: retain completed vector member */
#define XTC_DST_BUG_SCHEDFAIR  15  /* sched.c: bypass mandatory fairness override */
#define XTC_DST_BUG_BATCHDROP  16  /* batch.c: drop a held mt hand-in */
/*
 * 29 AIO_TAG_AFTER_DONE: src/io/io_sim.c's AIO reaper publishes done and
 * only then reads a->tag, the order every native reaper had until 2.1.2.
 * The xtc_aio_t is on the parked fiber's stack, and a kill-woken fiber
 * that sees done leaves the wait and dies, so the late read is of a dead
 * frame.  With the io.aio.wake_after_done buggify (the reaper stalls
 * between the two stores) test_sim_aio_migrate's kill arm reports "aio
 * reaper read a released xtc_aio_t".
 */
#define XTC_DST_BUG_AIO_TAG_AFTER_DONE 29

/*
 * 24 RECOV_REWALK: proc.c walks the recovery registry the 2.1.1 way
 * (indexed, n_recov zeroed after the loop, unmasked on the fault branch
 * and in xtc_proc_recovery_cleanup), so a kill that lands while a record
 * parks unwinds into the exit path, which runs the records above it a
 * second time (test_sim_scope: a recovery record ran twice).
 */
#define XTC_DST_BUG_RECOV_REWALK 24

/*
 * 25 SCOPE_UNMASKED_FIN: xtc_scope_leave detaches the recovery marker
 * first and runs the finalizers unmasked (2.1.1), so a kill delivered
 * while a finalizer parks drops every older finalizer of that scope
 * (test_sim_scope: a scope finalizer was skipped).
 */
#define XTC_DST_BUG_SCOPE_UNMASKED_FIN 25

/*
 * 26 SUP_LOCKPARK: sup.c's group-restart wait sleeps holding a pthread
 * mutex taken with __xtc_mtx_lock, as 2.1.1 held sup->lock there.  Any
 * fiber that parks inside an internal-lock bracket trips the sim's
 * step-boundary check (exec.c: "a fiber yielded holding an internal
 * lock"), which is what test_sim_sup_strategy's one_for_all case reports.
 */
#define XTC_DST_BUG_SUP_LOCKPARK 26

/*
 * 27 SUP_NOHOOK: sup.c runs its shutdown (kill the children, wait for
 * them, signal `stopped`) inline after its receive loop, as 2.1.1 did,
 * instead of from an xtc_proc_at_exit hook, so a supervisor that is
 * killed leaves its children running and never reports stopped
 * (test_sim_sup_strategy: "children outlived a killed supervisor").
 */
#define XTC_DST_BUG_SUP_NOHOOK 27

/*
 * 28 LAUNCH_EATMAIL: xtc_launch receives ANY message while it waits for
 * its child's DOWN and frees what is not that DOWN, as 2.1.1 did, so a
 * DOWN or other mail queued for the caller is lost (test_sim_launch: "a
 * message queued for the launcher did not survive xtc_launch").
 */
#define XTC_DST_BUG_LAUNCH_EATMAIL 28

#define XTC_DST_BUG_PIDEXEC 30     /* proc.c: 2.1 exec-relative pids */
#define XTC_DST_BUG_EXITNEG 31     /* proc.c: reason+1 kill/exit encoding */
#define XTC_DST_BUG_FAULTKIND 32   /* proc.c: user-frame fault reads EXIT */
#define XTC_DST_BUG_KILLKIND 33    /* proc.c: a kill reads EXIT */
#define XTC_DST_BUG_INITEXIT 34    /* svr.c: failed init returns CLEAN */

/*
 * 35 SUP_HOOK_SIBLING: a one_for_all / rest_for_one restart never
 * reports the siblings it killed to on_child_down (their DOWNs match no
 * live child and are dropped, as every DOWN of a killed sibling was before
 * 2.2), so the hook misses deaths (test_sim_sup_strategy: "on_child_down
 * missed").
 */
#define XTC_DST_BUG_SUP_HOOK_SIBLING 35

/*
 * 44 APP_REAPER_LATE: xtc_app_start spawns the registry's reaper but does
 * not publish its pid; the reaper publishes it when it first runs, as a
 * hand-spawned reaper does.  A child on another loop that calls
 * xtc_reg_put before loop 0 runs the reaper gets a plain,
 * unmonitored registration, and its name outlives it (test_sim_app: "a
 * monitored name outlived its proc").
 */
#define XTC_DST_BUG_APP_REAPER_LATE 44

/*
 * 45 DEMON_INFLIGHT: a DOWN that is in flight (sent, not yet queued: the
 * simulator's cross-loop latency) when the watcher calls xtc_demonitor is
 * still queued afterwards: the delivery ignores the cancel.  The watcher
 * then holds a DOWN for a monitor it turned off (test_sim_demonitor: "a
 * DOWN arrived after xtc_demonitor").
 */
#define XTC_DST_BUG_DEMON_INFLIGHT 45

/*
 * 46 REG_WAIT_NOWAKE: a registration does not wake xtc_reg_whereis_wait
 * callers, so a client waiting for a restarted child's name finds it only
 * when its timeout expires (test_sim_sup_name: "a name wait timed out
 * with the name registered").
 */
#define XTC_DST_BUG_REG_WAIT_NOWAKE 46

/*
 * 47 SUP_NAME_STALE: the supervisor does not drop a child's name when the
 * child dies (the 2.1 shape, where nothing did unless a reaper was
 * running), so between the death and the next registration the name
 * names a dead pid (test_sim_sup_name: "the registry named a dead
 * incarnation").
 */
#define XTC_DST_BUG_SUP_NAME_STALE 47

/*
 * 48 ROOM_SAVEQ: a receive that takes its message from the selective-
 * receive save queue does not wake the senders waiting for room, so a
 * sender blocked in xtc_send_wait on a receiver that drains with
 * xtc_recv_match stays parked (test_sim_send_wait: "a sender waited with
 * room in the mailbox").
 */
#define XTC_DST_BUG_ROOM_SAVEQ 48

/*
 * 49 SIM_DROP_LATE: a send the simulator's net latency puts in flight is
 * admitted without a look at the mailbox (the 2.1 shape) and, when it
 * lands in a mailbox that filled meanwhile, dropped although its sender
 * was told XTC_OK (test_sim_send_wait: "a message sent with XTC_OK was
 * lost").
 */
#define XTC_DST_BUG_SIM_DROP_LATE 49

/*
 * 50 FDONLY_STALE: xtc_recv arms its waker without clearing the
 * mail-blind bit an earlier XTC_WAIT_FD_ONLY wait set, so a receive
 * after such a wait is never woken by mail (test_sim_wait_fd_only: "a
 * receive after a mail-blind wait was never woken").
 */
#define XTC_DST_BUG_FDONLY_STALE 50

/*
 * 51 FDREADY_EDGE: io_sim.c reports a scheduled readiness
 * (xtc_sim_fd_ready_at) once and forgets it, an edge where the contract
 * is a level, so a proc that waits on the fd after the first waiter
 * consumed it never wakes (test_sim_fd_ready: "a waiter after the due
 * time never saw the readiness").
 */
#define XTC_DST_BUG_FDREADY_EDGE 51

/*
 * 52 EXT_NOWAIT: an external completion's due event runs without waiting
 * for xtc_sim_external_complete, so a slow producer's delivery is lost
 * (test_sim_external: "an external completion was not delivered").
 */
#define XTC_DST_BUG_EXT_NOWAIT 52

/*
 * 53 EXT_LANE: xtc_sim_external_begin ignores the lane, so a FIFO lane's
 * later completion can be delivered before an earlier one
 * (test_sim_external: "a lane delivered out of order").
 */
#define XTC_DST_BUG_EXT_LANE 53

/* rcu.c: a fiber's cached reader slot, no generation check. */
#define XTC_DST_BUG_RCU_SLOT_GEN 54

/*
 * 63 RCU_NOSLOT: a read-side that could not get a reader slot (out of
 * memory) is not counted, so synchronize does not wait for it and a
 * writer frees a node under it (test_sim_rcu: "a slotless reader saw a
 * freed node").
 */
#define XTC_DST_BUG_RCU_NOSLOT 63

/*
 * 64 RCU_RETIRE_FREE: a retirement that cannot be recorded (out of
 * memory) frees the object at once instead of leaking it, under the
 * readers that still hold it (test_sim_rcu: "an unrecorded retire freed
 * a node under a reader").
 */
#define XTC_DST_BUG_RCU_RETIRE_FREE 64

/*
 * 65 LRLOCK_OPLOG_DROP: xtc_lrlock_apply_op returns without applying an
 * op the oplog cannot grow to hold, so the write is lost from both
 * copies (test_sim_lrlock: "a write was lost").
 */
#define XTC_DST_BUG_LRLOCK_OPLOG_DROP 65

/*
 * 60 SUP_WAIT_SKIP: a supervisor started with wait_children does not wait
 * for a server or supervisor child's start: it spawns the next child at
 * once and reports the start as done, so a child whose init fails is not
 * a start failure, and a later child runs before an earlier one is up
 * (test_sim_sup_child: "a child ran before the one before it was up").
 */
#define XTC_DST_BUG_SUP_WAIT_SKIP 60

/*
 * 61 SUP_NESTED_CLEAN: a nested supervisor (xtc_sup_child_main) that
 * spends its restart budget exits cleanly, so a TRANSIENT parent spec
 * never restarts the subtree and the service stays down
 * (test_sim_sup_child: "a nested supervisor's give-up did not reach its
 * parent").
 */
#define XTC_DST_BUG_SUP_NESTED_CLEAN 61

/*
 * 62 SUP_NESTED_NODRAIN: a nested supervisor drops XTC_APP_SHUTDOWN_MSG
 * instead of passing it to its children, so an app drain force-cancels
 * the subtree (test_sim_sup_child: "a drain did not reach a nested
 * supervisor's child").
 */
#define XTC_DST_BUG_SUP_NESTED_NODRAIN 62

/*
 * 67 PART_SYSDROP: the simulator's partition cuts system messages too, so
 * a monitor's DOWN or a link's EXIT from a proc on an isolated loop is
 * dropped for good (test_sim_partition: "a DOWN or link EXIT was lost
 * under a partition").
 */
#define XTC_DST_BUG_PART_SYSDROP 67

/*
 * 68 SIM_OFFLOOP_WAIT: a wait on the simulator's own thread off a fiber
 * (an xtc_sim_external_complete delivery fn) behaves as in 2.2.0: a
 * timed wait takes its deadline from the wall clock and xtc_send_wait
 * waits for room no fiber can make, so a delivery fn that meets a full
 * mailbox trips the determinism guard (timed) or blocks the run for
 * good (-1) (test_sim_external: "a delivery fn's wait did not return
 * at once").
 */
#define XTC_DST_BUG_SIM_OFFLOOP_WAIT 68

/*
 * 69 BATCH_OWNER_KEEP: the at-exit hook of a proc that created a batch
 * group on its running loop does nothing, so the dead owner's group stays
 * open: its members never end and its driver never finishes
 * (test_sim_batch: "a dead owner's group was not closed").
 */
#define XTC_DST_BUG_BATCH_OWNER_KEEP 69

/*
 * 70 BATCH_PREEMPT_KEEP: xtc_batch_preempt moves the member back to the
 * waiting list but keeps its reservation, so the units it was taken back
 * for are not free (test_sim_batch: "preempt did not free the units").
 */
#define XTC_DST_BUG_BATCH_PREEMPT_KEEP 70

/*
 * 71 BATCH_RECHARGE_OVER: xtc_batch_recharge grows a reservation without
 * checking the free units, so the group reserves more than
 * resource_capacity (test_sim_batch: "reservation over capacity").
 */
#define XTC_DST_BUG_BATCH_RECHARGE_OVER 71

/*
 * 76 RMARK_LATE: xtc_recv_match_since starts after what has arrived by
 * the time of the receive, not by the time of xtc_recv_mark, so a reply
 * that arrived after the mark and before the receive is skipped
 * (test_sim_recv_mark: "a marked receive skipped a reply").
 */
#define XTC_DST_BUG_RMARK_LATE 76

/*
 * 77 SUP_FORGED_DOWN: a supervisor acts on any message shaped like a
 * DOWN, not only one the library sent, so an xtc_send of a DOWN's bytes
 * naming a live child restarts it while the first copy runs on
 * (test_sim_forged_down: "a forged DOWN restarted a live child").
 */
#define XTC_DST_BUG_SUP_FORGED_DOWN 77

/*
 * 78 REG_FORGED_DOWN: the registry reaper acts on any message shaped like
 * a DOWN, so an xtc_send of a DOWN's bytes naming a live proc drops its
 * names (test_sim_forged_down: "a forged DOWN dropped a live name").
 */
#define XTC_DST_BUG_REG_FORGED_DOWN 78

#endif /* XTC_DST_INJECT_H */
