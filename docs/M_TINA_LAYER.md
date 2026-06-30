# A Tina-flavored Isolate layer on libxtc -- feasibility + architecture

Question: can a "Tina-like" C layer be built ON TOP of libxtc that gives
C programs the full Tina experience -- Isolates, Effects/Transitions,
shard-per-core, generational handles, bounded mailboxes, supervision,
the "handler returns an Effect" model -- with libxtc as the foundation?

Verdict: YES, as a distinct product ("Tina-on-xtc"), ~85% faithful at
the API surface.  The decisive architectural choice is to map
Shard -> libxtc proc, NOT Isolate -> proc.

Status: this layer is now a **supported part of libxtc** (it was
promoted out of `examples/08_tnt/`).  The implementation is
`src/orc/tnt.c` (L4 orchestration, alongside svr/sup/app), the public
header is `src/inc/xtc_tnt.h`, the manual page is
`man/man3/xtc_tnt.3`, and the tests live in `test/tnt/`
(`test_tnt.c`, `test_tnt_echo.sh`) and run under `make check`.  The
public surface is prefixed `xtc_tnt_*` to match library convention;
the 64-bit generational handle keeps its bit-identical Tina layout.
The canonical TCP echo server remains in `examples/08_tnt/echo.c`,
linking the in-library layer via `<xtc_tnt.h>`.

## The crux: stackless Isolates vs stackful procs

Tina's Isolate is a stackless state machine: a typed struct in a dense
arena, a generational handle, a handler that runs to completion and
returns a 2-byte Transition/Effect.  libxtc's xtc_proc is a stackful
coroutine (a fiber with a saved stack).  These are different execution
models.

WRONG mapping (rejected): one xtc_proc per Isolate.  Each proc carries a
fiber stack (kilobytes), so you lose Tina's millions-of-256-byte-
Isolates density, and you cannot do Tina's type-pure arena batching.

RIGHT mapping: one long-lived xtc_proc per SHARD (one per xtc_exec
loop).  That proc owns the typed arenas (slab-carved at boot), the
per-Isolate metadata + free list, and runs Tina's exact dispatch loop:
drain cross-shard inbox -> collect reactor completions -> for each ready
type, for each dispatchable slot (budgeted): call handler -> interpret
the returned Effect (commit staged I/O into xtc_aio / net, enqueue
sends, arm timers).  The handler NEVER blocks; the single fiber stack
belongs to the shard.  The stackful underlay becomes an ASSET (natural
reactor parking of the shard fiber) and an irrelevance at the Isolate
level.

Cost: you re-implement Tina's scheduler (shard.odin, ~3k lines) in C.
That is the real labor.  libxtc gives you the executor threads, the
reactor, the fault trap, the allocator, and the resource accountant --
the substrate -- but NOT the Isolate scheduler.

## What maps, and how faithfully

EXACT (rebuild in the layer, no libxtc constraint blocks it):
  - handler(self, msg) -> Effect signature; ctx_* stage-then-commit
    turn frame; the 2-byte transition.
  - generational handles (xtc_pid_t is already generational; define your
    own u64 packing with a type field).
  - backpressure with immediate sender feedback (xtc_proc's mailbox
    already does refuse-at-cap + drop count; drop-on-full is a one-line
    policy choice in your enqueue).
  - bounded everything (xtc_res caps, slab oom policy).
  - zero-allocation supervision (direct calls over the arena, as Tina
    does -- not via xtc_orc, which supervises procs).
  - I/O as effects (the Effect enum is layer code; the backends are
    xtc_aio / net / timers).

NEAR-EXACT:
  - thread-per-core (xtc_exec + xtc_shard_id; disable work stealing,
    add affinity).
  - segfault containment: xtc_proc_recovery_arm + the recovery registry
    ARE Tina's exact mechanism (sigaltstack+siglongjmp / VEH), runtime-
    verified.  A segfault unwinds the SHARD fiber -> shard rebuild,
    which is precisely Tina's Level-2 recovery (Tina also escalates a
    real SIGSEGV to shard rebuild).  Voluntary crashes are Level-1 (tear
    down one arena slot, no trap).

APPROXIMATE:
  - no-malloc-after-boot: achievable in PRACTICE by carving all memory
    from boot-time xtc_slab caches and never calling the mallocing
    libxtc APIs (xtc_recv mallocs; the shard reads its own arena mailbox
    directly).  Enforced by discipline + caps, not structurally (C has
    malloc).
  - three memory generations (shard-permanent slab / per-slot working /
    per-turn scratch via a reset bump arena).

NOT (or not worth attempting):
  - byte-identical DST via interpreter swap: libxtc's own DST
    (docs/M_DST.md) is a different model (seeded scheduling of real
    fibers) and is being built separately.  BUT if the layer keeps the
    "handlers only return Effects, never block" invariant, you can write
    Tina-style DST IN THE LAYER (swap the effect interpreter for a
    scripted one) independent of libxtc's DST.
  - lock-free SPSC cross-shard: xtc_send is mutexed; match Tina by
    building per-pair xtc_chan_mpsc rings, or accept the mutexed
    transport as "approximate".  Intra-shard is single-fiber so it is
    atomic-free for free.
  - the structural absence of a general allocator (C always has malloc).

## Proposed API (namespace xtct_)

Public: xtct_handle_t (u64 shard:type:slot:gen), xtct_transition_t
(2-byte kind+reason), xtct_message_t, xtct_type_t (the IsolateType
descriptor: stride, slot_count, working_memory_size, mailbox_capacity,
budget_weight, init/handler fns), xtct_spec_t (the SystemSpec),
xtct_start(spec).  Ambient ctx_* via a thread-local current shard/frame
(libxtc already has the thread-local current-proc notion): xtct_send,
xtct_spawn, xtct_submit_io, xtct_io_send, xtct_register_timer,
xtct_scratch_arena, xtct_working_arena.  The effect interpreter commits
staged effects into xtc_aio / net / timers after the handler returns.

Boot: xtc_exec_init(shard_count) + service mode + per-shard slab arenas
+ xtc_exec_spawn_on(loop_i, shard_main) + xtc_exec_run.

## What libxtc-as-foundation buys you over from-scratch

A proven portable reactor across 6 backends (CI-tested under ASan/UBSan)
on more platforms than Tina; the fault trap already built and runtime-
verified; resource governance (xtc_res); a slab allocator with shared-
memory mode (enabling cross-process Isolates, which Tina cannot do); a
thread-per-core executor with cross-thread wakeup already solved.

## Recommendation

Build Option B (Shard -> proc).  Budget the bulk of effort for the
scheduler port (shard.odin -> C), not for the libxtc integration (mostly
plumbing xtc_exec / reactor / trap / slab into a shard main).  Ship
intra-shard drop-on-full exactly; ship cross-shard as xtc_chan_mpsc
rings if faithfulness matters.  Declare DST and no-malloc-after-boot as
"layer-enforced, best-effort" to stay honest against Tina's stronger
structural claims.  The result is Tina's IDEAS on libxtc's MACHINERY --
which is exactly what "a Tina-like layer on libxtc" should mean.
