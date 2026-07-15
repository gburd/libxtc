# Waker register/wake data race (pre-existing) -- FIXED 2026-07

## STATUS: FIXED (commit follows this doc).  6/6 TSan runs of
## test_proc_table_stress now clean; messaging suite TSan+ASan clean;
## DST 55/55 replay-identical; gcc -Werror clean.

## Root cause (the real one -- deeper than the first hypothesis)
The race was NOT just the lockless ARM in __do_recv (that was one write
site).  The decisive bug was the READ side: __mbox_deliver reads
p->waker_armed UNDER mbox_lock, UNLOCKS, then reads p->recv_waker
OUTSIDE the lock to fire the wake (the wake must run outside the lock
to avoid proc/loop reentrancy).  Meanwhile the woken receiver, on its
NEXT recv cycle, RE-ARMS recv_waker under its own lock.  So the
sender's post-unlock read of recv_waker raced the receiver's re-arm
write -- a genuine C11 data race (benign in value, since recv_waker is
always the same fixed self->task/loop, but real UB).

## Fix (two parts)
1. Arm recv_waker together with waker_armed=1, both UNDER mbox_lock, at
   the park points in __do_recv and xtc_proc_wait_fd (was written
   lockless just before).
2. In __mbox_deliver, COPY recv_waker into a local while still holding
   mbox_lock (guarded by `armed`), then fire xtc_waker_wake(&local)
   after unlocking.  The wake still runs outside the lock (no
   reentrancy), but the struct READ is now under the lock, so it can
   never race the receiver's re-arm.
The other wake sites (xtc_exit_pid / kill at proc.c ~1482, ~1530)
already read recv_waker INSIDE mbox_lock, so they were race-free and
are unchanged.

## Why the first attempt (arm-under-lock alone) was insufficient
Moving only the arm under the lock left the sender's post-unlock read
racing the NEXT re-arm; 4/5 TSan runs still tripped.  Reading the waker
under the lock (part 2) is what actually closes it.

## test disposition
test_proc_table_stress is now a CLEAN TSan gate for the waker path.

---
(historical analysis below)

## What
test/concurrency/test_proc_table_stress (the new striped-proc-table
stress) surfaces a TSan data race in the waker path under heavy
concurrent recv-vs-cross-thread-send:

  WRITE  xtc_task_waker (task.c:205)  <- receiver in __do_recv registers
         self->recv_waker.{loop,task}
  READ   xtc_waker_wake (task.c:225)  <- sender in __mbox_deliver reads
         p->recv_waker.{loop,task} to fire the wake

## Why it is NOT the striping change
Confirmed: the uncommitted 19.5c diff touches only the per-loop TABLE
lock (t->lock -> t->stripes[]) and the proc-slab DCL init.  It does NOT
touch recv_waker, xtc_task_waker, or __mbox_deliver's unlock-then-wake.
git diff shows zero '+' lines mentioning the waker path.  The race is
pre-existing; the new stress (8 threads x 2000 sends to 64 receivers
continuously re-arming) is simply the first test aggressive enough to
hit it.  All the gentler messaging tests (test_proc/svr/reg/sup) stay
TSan-clean.

## Why it is BENIGN in value (but still real UB)
recv_waker.loop and .task are set to the SAME fixed values every time
(the proc's own task and its loop), so even a torn read yields correct
values.  But it is a genuine C11 data race: the register in __do_recv
runs (outside or inside self->mbox_lock -- doesn't matter) while the
wake in __mbox_deliver reads recv_waker AFTER deliberately UNLOCKING
p->mbox_lock (the wake must run outside the lock to avoid nesting /
reentrancy -- see proc.c ~837).  So the two sides never share a lock;
moving the register under mbox_lock does not fix it (tested).

## The correct fix (its OWN change, touches task.c + every waker user)
Two clean options:
1. Initialize recv_waker ONCE at proc setup (p->task assignment in
   __proc_entry / __proc_spawn_core) instead of re-arming it on every
   __do_recv, with ordering that guarantees it is written before the
   proc is discoverable by any sender.  Zero per-recv work; but needs
   care about the spawn-vs-first-deliver window.
2. Make xtc_waker_t.{loop,task} _Atomic and use relaxed
   store/load in xtc_task_waker / xtc_waker_wake.  Removes the C11
   race with no ordering assumptions and no behavior change, but edits
   the shared xtc_waker_t struct + task.c, so every waker consumer
   (io wait, chan, sync, proc) is in scope -> re-run the full suite +
   TSan.

Recommend option 2 (smallest, provably race-free, no window reasoning).
Deferred to its own session with a full TSan pass over the waker
consumers.  NOT a 19.5c blocker: the striping change is independently
correct (ASan-clean, conservation exact, all non-stress tests
TSan-clean); this race predates it.

## Test disposition meanwhile
test_proc_table_stress stays in TESTS_C (it proves routing + no-UAF
under ASan and the conservation invariant).  Under TSan it currently
reports this pre-existing benign race; once option 2 lands, it becomes
a clean TSan gate for the waker path too.
