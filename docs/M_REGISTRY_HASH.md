# M_REGISTRY_HASH -- Hash-table upgrade for the process registry

## 1. Scope and goal

The process registry (`src/orc/reg.c`, header `src/inc/xtc_reg.h`)
maps a string `name -> xtc_pid_t`. Today it is a linear array under a
mutex; `src/orc/reg.c:7` says so explicitly:

> "M10.5: simple linear table under a mutex. M11+ swaps in xtc_chash
> (RCU hash table) for wait-free reads."

Lookup, register, and unregister are all O(n) (see
`__reg_find_locked` at `src/orc/reg.c:54-61`, called from
`xtc_reg_register` `:79`, `xtc_reg_unregister` `:99`, and
`xtc_reg_whereis` `:117`). This document designs the internal
upgrade to O(1) average-case while preserving the public API exactly.

**Finding (important):** `xtc_chash` referenced by the comment **does
not exist anywhere in the tree.** A `find` for `**/*chash*` returns
nothing; the only references are aspirational prose in
`PLAN.md:2992` ("RCU-protected concurrent hash table (`xtc_chash`) --
primary RCU consumer. M13a."), `src/inc/xtc_cfg.h:25`,
`src/inc/xtc_rcu.h:8`, and the `reg.c:7` comment itself. So the
"swap in xtc_chash" plan was never realized. We have two honest
choices: build a general `xtc_chash` first (large, out of scope), or
embed a small purpose-built chained hash table directly in `reg.c`
matching the conventions already proven in `lock_mgr.c` and
`alloc_audit.c`. **This design takes the second path** and explains
why in section 5.

## 2. Requirements and constraints

- **Public API frozen.** `src/inc/xtc_reg.h:26-31` carries the
  `PUBLIC:` markers that drive the symbol map / `__P` prototype
  extraction (the same mechanism `test/m0/test_symbols.sh` checks per
  `dist/libxtc.map:11`). The six signatures --
  `xtc_reg_create/destroy/register/unregister/whereis/count` -- must
  not change. The change is confined to `struct xtc_reg` and the
  function bodies in `reg.c`.
- **`struct xtc_reg` is opaque** (forward-declared at
  `xtc_reg.h:23`, defined only in `reg.c:21-26`), so its layout is
  free to change.
- **Allocator discipline.** Use the `__os_*` vtable
  (`src/inc/os_alloc.h`): `__os_calloc`, `__os_free`, `__os_strdup`,
  `__os_realloc`, and -- only if over-aligned -- `__os_aligned_alloc`
  / `__os_aligned_free` (see section 6).
- **BSD KNF** per `.clang-format`, ASCII-only (AGENTS.md).
- **Existing test must keep passing**: `test/m10/test_reg.c`,
  wired in `dist/Makefile.in:311,987-991`.

## 3. The two existing hash tables (conventions to match)

### 3.1 lock_mgr.c -- FNV-1a, fixed bucket array, chained

- FNV-1a 32-bit, `src/ptc/lock_mgr.c:166-172`:
  ```
  uint32_t h = 2166136261u;        /* FNV offset basis */
  for (i = 0; i < size; i++) { h ^= p[i]; h *= 16777619u; }  /* FNV prime */
  return h ? h : 1;                /* never return 0 */
  ```
- Fixed bucket array, chained collision lists. The locker table is
  literally `struct locker_rec *locker_table[256]`
  (`lock_mgr.c:154`), indexed by `id & 0xff` (`:181`,
  `__locker_find` `:177-185`), insert at head (`:199-201`),
  unlink-by-link-pointer on delete (`__locker_id_free` `:223-235`).
- The object hash uses `h % n_parts` partitioning (`lock_get` `:445`)
  with per-partition chains walked by `memcmp` of the key
  (`__obj_lookup` `:281-289`).

### 3.2 alloc_audit.c -- pointer-keyed, fixed power-of-two array, chained

- `#define AUDIT_BUCKETS 16384u` "power of two"
  (`src/ptc/alloc_audit.c:35`), buckets are `struct rec **g_buckets`
  (`:41`).
- Index = mix-then-mask: `(v * 0x9E3779B97F4A7C15) >> 32 & (N-1)`
  (`__bucket` `:46-51`) -- mask because power-of-two.
- Insert at head (`__rec_insert` `:64-79`), unlink-by-link on remove
  (`__rec_remove` `:84-103`), free chains in a `for b ... while r`
  teardown (`:177-181`). Whole thing under one `pthread_mutex_t g_mu`
  (`:38`).

**Both are fixed-size chained tables under a plain mutex. Neither
resizes. Neither uses RCU.** That is the house style. The registry
should look like a sibling of these.

## 4. Hash structure: chained, FNV-1a, fixed power-of-two buckets

**Decision: separate chaining with a fixed power-of-two bucket array,
FNV-1a over the name string.** Rationale:

- **Match the codebase.** Open addressing would be a novel pattern
  here; both existing tables chain. Chaining also makes per-node RCU
  retirement trivial (section 5.2) -- an open-addressing table cannot
  free a single slot without tombstones and cannot be read locklessly
  during a backward-shift delete.
- **FNV-1a for strings** reuses the exact constants in
  `lock_mgr.c:169,171` (offset basis `2166136261u`, prime
  `16777619u`). The registry key is a NUL-terminated name rather than
  a sized buffer, so iterate to `'\0'`:
  ```
  static uint32_t
  __reg_hash(const char *s)
  {
      uint32_t h = 2166136261u;
      for (; *s != '\0'; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
      return h;                    /* 0 is fine; index masks it */
  }
  ```
  (No `h ? h : 1` needed -- `lock_mgr.c` reserves 0 because it stores
  the hash in the node and compares it; we can store it too for fast
  chain rejection, in which case keep any value, including 0.)

### Node and table layout

```
struct reg_node {
    struct reg_node *next;   /* chain link (RCU-published; see 5.2) */
    uint32_t         hash;   /* cached FNV-1a, fast chain reject */
    xtc_pid_t        pid;
    char            *name;   /* __os_strdup'd, owned by node */
};

struct xtc_reg {
    pthread_mutex_t   lock;      /* writers only (register/unregister) */
    struct reg_node **buckets;   /* REG_NBUCKETS pointers */
    int               n;         /* live entry count (xtc_reg_count) */
};
```

Index = `hash & (REG_NBUCKETS - 1)` (mask, power-of-two, as in
`alloc_audit.c:50`).

### 4.1 Bucket count and resize policy

**Decision: fixed size, no resize. `REG_NBUCKETS = 256`
(power of two).**

- Both reference tables are fixed (256 and 16384). The registry is a
  *named-service* table -- Erlang's "register a gen_server under a
  stable name" pattern (`xtc_reg.h:8-12`). Real apps register on the
  order of tens, occasionally hundreds, of names (one per long-lived
  service), not millions. 256 buckets keep the average chain at or
  below ~1 even for a few hundred names; at 1000 names the average
  chain is ~4, still O(1) amortized and far better than today's O(n)
  linear scan of the whole table.
- A resize during lockless RCU reads is the genuinely hard case (you
  must publish a new bucket array and retire the old one through a
  grace period while readers may hold pointers into either). Skipping
  resize removes that hazard entirely. `lock_mgr.c` and
  `alloc_audit.c` both made this same call.
- 256 matches `lock_mgr.c`'s `locker_table[256]` exactly, so a
  reviewer sees a familiar number.

`ponytail: fixed 256 buckets, no resize. If a deployment ever
registers tens of thousands of names, bump REG_NBUCKETS or add a
one-shot grow-on-first-overflow -- but only when a real workload
shows chains long enough to matter. The scale test in section 9
proves O(1) up to thousands at 256.`

## 5. Concurrency

The registry is **read-mostly**: `xtc_reg_register` /
`xtc_reg_unregister` fire at service startup/shutdown (rare);
`xtc_reg_whereis` fires on every "send to a named service" (hot).
Today all three serialize on one mutex (`reg.c:84,99,116,128`).

### Option A -- keep the mutex, just make lookup O(1)

Replace the linear scan with a hashed bucket walk; otherwise keep the
single `pthread_mutex_t`. `whereis` still takes and releases the lock,
but the critical section drops from O(n) to O(chain length) ~ O(1).

- Pros: smallest diff; trivially correct; identical to
  `alloc_audit.c`'s model; no RCU lifetime reasoning; no dependency
  on `xtc_rcu` init/fini ordering.
- Cons: concurrent lookups still contend on one lock. For a registry
  hit on a hot path from many loops/threads, the mutex is a
  serialization point even though nothing is being modified.

### Option B -- RCU read-side, mutex-guarded writes

Lockless `whereis` via `xtc_rcu_read_lock/unlock`
(`src/ptc/rcu.c`, `src/inc/xtc_rcu.h`); writers still take the mutex,
publish via release-store, and retire removed nodes through
`xtc_rcu_retire` + a grace period.

- Pros: wait-free concurrent reads -- exactly the "wait-free reads"
  the `reg.c:7` comment promised, and the stated intent in
  `PLAN.md:2992` (registry as "primary RCU consumer"). Reads from any
  number of threads never contend.
- Cons: real lifetime complexity (detailed below); depends on
  `xtc_rcu_init`/`xtc_rcu_fini` being called in the process lifecycle;
  `xtc_rcu_synchronize` blocks the *writer* (acceptable -- writes are
  rare); the RCU layer's reclamation lags by two epochs
  (`rcu.c:236`, and the M13 test calls `synchronize` three times to
  flush, `test/m13/test_rcu.c:48-52`), so a destroyed registry must
  force reclamation at teardown.

### Recommendation: **B (RCU reads) -- but stage it behind A.**

Ship **Option A first** as the mechanical, obviously-correct change
(hash the bucket, keep the mutex), land it, confirm `test_reg` and CI
green. Then layer **Option B** as a follow-on once the RCU read
protocol below is reviewed. The two share the same node/bucket
layout, so A is a strict subset of B -- no rework, just removing the
read-side mutex and adding retire/synchronize on the write path.

Reasoning: the value here is read scalability on the hot `whereis`
path, and the runtime already ships `xtc_rcu` with reclamation
verified in `test/m13/test_rcu.c`. Using it fulfills the original
design intent rather than leaving an O(1)-but-still-contended table.
But correctness-first means the lock-only version is the safe floor;
RCU is the optimization, gated on review.

If a single milestone is preferred, implement B directly -- the
protocol is fully specified below.

### 5.1 RCU read-side protocol (Option B)

```
int
xtc_reg_whereis(xtc_reg_t *r, const char *name, xtc_pid_t *out_pid)
{
    uint32_t h;
    struct reg_node *node;
    int rc = XTC_E_INVAL;

    if (r == NULL || name == NULL || out_pid == NULL) return XTC_E_INVAL;
    h = __reg_hash(name);

    xtc_rcu_read_lock();
    /* acquire-load the bucket head, then walk the chain; every
     * 'next' was published with a release-store under the writer
     * mutex, so the acquire here gives us a consistent chain. */
    for (node = atomic_load_explicit(&r->buckets[h & (REG_NBUCKETS-1)],
             memory_order_acquire);
         node != NULL;
         node = atomic_load_explicit(&node->next, memory_order_acquire)) {
        if (node->hash == h && strcmp(node->name, name) == 0) {
            *out_pid = node->pid;   /* xtc_pid_t is a small POD: copy by value */
            rc = XTC_OK;
            break;
        }
    }
    xtc_rcu_read_unlock();
    return rc;
}
```

Key points:

- `buckets[i]` and `reg_node.next` become `_Atomic(struct reg_node *)`
  so the publish/consume is a defined data race-free handoff (the same
  acquire/release discipline `rcu.c` uses internally, e.g.
  `rcu.c:181-186`).
- The read-side copies `pid` *by value* into the caller's `*out_pid`
  while inside the read section. `xtc_pid_t` is a 8-byte POD
  (`xtc_proc.h:34-38`: two `uint16_t` + one `uint32_t`), so this is a
  cheap struct copy and the node may be reclaimed the instant we
  `read_unlock`. We never hand a pointer *into* the node back to the
  caller -- so reclamation can never UAF the caller.
- `xtc_rcu_read_lock` lazily registers the calling thread
  (`rcu.c:131-145`); first call from a new thread allocates a TLS slot.
  No init needed at read time, but see 5.4 for `xtc_rcu_init`.

### 5.2 Writers and grace-period reclamation (Option B)

Both writers take `r->lock` (writers serialize among themselves; only
readers are lockless).

`xtc_reg_register`:
1. Lock `r->lock`.
2. Walk the target bucket (plain pointer walk is fine under the lock;
   no other writer runs) to reject duplicates -> `XTC_E_INVAL`,
   matching `reg.c:80`.
3. `__os_strdup` the name into a freshly `__os_calloc`'d node, set
   `pid` and `hash`.
4. `node->next = buckets[i]` (plain store; node not yet visible).
5. **Publish**: `atomic_store_explicit(&buckets[i], node, release)`.
   A concurrent reader either sees the old head (misses the new node
   -- acceptable, it wasn't registered yet from its viewpoint) or the
   new head with a fully-initialized node.
6. `r->n++`; unlock.

`xtc_reg_unregister`:
1. Lock `r->lock`.
2. Find the node by walking the bucket, tracking the predecessor's
   `next` slot (the `**link` idiom from `alloc_audit.c:90-100` /
   `lock_mgr.c:228`).
3. **Unlink**: `atomic_store_explicit(link, node->next, release)`.
   New readers entering after this never see the node; readers
   already past `link` may still hold `node`.
4. `r->n--`; unlock (drop the writer lock before the grace period --
   no need to hold it while waiting).
5. **Retire**: hand the node to RCU for deferred free:
   ```
   xtc_rcu_retire(node, __reg_node_free);
   ```
   where `__reg_node_free(void *p)` does
   `__os_free(((struct reg_node*)p)->name); __os_free(p);`.
   The node (and its `name`) are freed only after every reader that
   could hold it has left its read-side (`rcu.c:189-244`).
   `xtc_reg_unregister` does **not** call `xtc_rcu_synchronize` itself
   -- it just retires and returns. Reclamation happens on the next
   epoch advance driven by any writer's synchronize or a periodic
   helper, exactly as the RCU contract states (`xtc_rcu.h:64-67`).

   `ponytail: retire-only on unregister. If a workload churns names
   fast enough that retired nodes pile up between grace periods,
   either call xtc_rcu_synchronize() once per unregister (simple,
   makes unregister O(grace period)) or run a periodic synchronize.
   Registry writes are rare, so retire-only is the right default.`

**Re-register-after-unregister** (the test does this,
`test/m10/test_reg.c:42-46`): unregister retires the old node; a
subsequent register allocates a brand-new node. The old node's memory
is not reused until its grace period elapses, so there is never a
window where a reader sees a half-recycled node. Correct by
construction.

### 5.3 Destroy and the two-epoch lag (Option B)

`rcu.c` reclaims a retired bucket only two epochs later
(`rcu.c:236`, and `test/m13/test_rcu.c:48-52` advances three times to
flush two retirees). So `xtc_reg_destroy` cannot just free the
buckets -- nodes retired moments before may still be pending in an RCU
bucket, and freeing the table out from under them is fine (RCU owns
their lifetime now) but we must not leak them.

`xtc_reg_destroy`:
1. Lock `r->lock` (defensive; by contract destroy races nothing).
2. For each bucket, for each node still linked: free directly
   (`__os_free(name)` then `__os_free(node)`) -- these were never
   retired, so we own them. This mirrors the teardown loops in
   `alloc_audit.c:177-181` and `lock_mgr.c` destroy.
3. Unlock, `pthread_mutex_destroy`, `__os_free(r->buckets)`,
   `__os_free(r)`.
4. **Nodes already retired** (unregistered before destroy) are owned
   by the global RCU machinery and are freed when `xtc_rcu_fini`
   drains all buckets (`rcu.c:108-130`) or by enough
   `xtc_rcu_synchronize` calls. That is a process-global concern, not
   the registry's -- destroying the registry must not free them.

This is the one genuine subtlety RCU adds. Document it at the
`xtc_reg_destroy` site.

### 5.4 RCU init/fini ownership (Option B)

`xtc_rcu_init` is idempotent (`rcu.c:97-103`) and auto-runs on first
`read_lock`/`retire`. The registry does **not** own RCU's lifecycle
-- the app/runtime does. The registry just uses it. No change to
`xtc_reg_create` beyond a defensive `(void)xtc_rcu_init();` is needed,
and even that is optional since reads self-register. Recommend a
single `(void)xtc_rcu_init();` in `xtc_reg_create` so an allocation
failure inside RCU surfaces early rather than on the first hot lookup
-- this mirrors the rationale in `xtc_rcu.h:48-51`.

## 6. Memory and ownership

- **Name string ownership unchanged.** Today `xtc_reg_register`
  duplicates the caller's name with `__os_strdup`
  (`reg.c:84`) and `xtc_reg_unregister` / `xtc_reg_destroy`
  `__os_free` it (`reg.c:46,103`). Keep this exactly: the node owns a
  `__os_strdup`'d copy; the caller retains its own string. This
  preserves the existing contract that callers may free or reuse the
  name buffer after `register` returns.
- **Node allocation:** `__os_calloc(1, sizeof(struct reg_node), ...)`
  -- the same primitive the current code uses for the registry struct
  (`reg.c:35`) and that `lock_mgr.c` uses for `locker_rec`
  (`lock_mgr.c:191`). `calloc` zeroes `next`, which is the correct
  initial chain terminator.
- **Bucket array:** `__os_calloc(REG_NBUCKETS, sizeof(struct
  reg_node *), ...)` -- zeroed null heads, like
  `alloc_audit.c:155`.
- **Over-alignment: not a concern here.** AGENTS.md requires
  `__os_aligned_alloc`/`__os_aligned_free` only for structs (or
  arrays of structs) with a member declared `_Alignas(XTC_CACHE_LINE)`
  stricter than `max_align_t`. Neither `struct reg_node`, `struct
  xtc_reg`, nor the bucket pointer array has any over-aligned member
  (`XTC_CACHE_LINE` is defined at `xtc_int.h:67` but we do not use the
  `XTC_CACHE_PAD` macro at `xtc_int.h:115` here). Plain `__os_calloc`
  / `__os_free` are correct and sufficient. **Do not** introduce
  cache-line padding on registry nodes -- there is no per-node hot
  counter to false-share, and it would force the aligned allocator for
  no benefit. If a future change adds an `_Atomic` per-bucket
  statistic on its own cache line, revisit per the AGENTS.md rule.
- Under Option B the `_Atomic` qualifier on `buckets[]` and
  `reg_node.next` changes no sizes and imposes no extra alignment on
  these platforms (pointer-sized atomics are naturally aligned).

## 7. API preservation

No header change. `src/inc/xtc_reg.h` stays byte-for-byte identical:
the six `PUBLIC:` prototypes (`xtc_reg.h:26-31`) and their plain
declarations (`:33-45`) are untouched, so `dist/libxtc.map` and
`test/m0/test_symbols.sh` see no new or removed symbols. Behavior
preserved:

- `register` rejects duplicate names with `XTC_E_INVAL`
  (`reg.c:80`, tested at `test/m10/test_reg.c:24`).
- `unregister` returns `XTC_E_INVAL` for an unknown name
  (`reg.c:92` default rc, tested at `test_reg.c:39`).
- `whereis` returns `XTC_E_INVAL` (not a distinct "not found" code)
  on miss (`reg.c:111`, tested at `test_reg.c:35,38`).
- `count` returns the live entry count (`reg.c:131`), now read from
  `r->n` -- still mutex-guarded under both options (it is a writer-ish
  read; keep the lock, or make it an `_Atomic int` under B). Tested at
  `test_reg.c:18,28,41`.
- Re-register after unregister succeeds with a new pid
  (`test_reg.c:42-46`).

All `XTC_E_*` values used are stable (`xtc.h:56-63`).

## 8. Liveness / stale-pid handling

**Finding: the registry does NOT validate pid liveness today, and
this design preserves that.** `xtc_reg_whereis` returns whatever pid
was registered (`reg.c:119`); a `grep` for `alive|pid_alive|->gen`
in `src/orc/` finds nothing in `reg.c`. The `xtc_pid_t` carries a
`gen` counter precisely so a *consumer* can detect staleness
(`xtc_proc.h:29-33`: "a stale pid ... is recognisably stale on
lookup"), and `xtc_send` already returns `XTC_E_INVAL` for a
"stale/unknown pid" (`xtc_proc.h` send contract). So the liveness
check lives at the send boundary, not in the registry. That is the
correct layering and must not change:

- The registry is a *name table*, not a liveness oracle. A registered
  service that dies leaves a stale entry until something
  unregisters it (an at-exit hook, a monitor, or a supervisor). This
  matches Erlang's `register/2`, where a dead name is cleaned up by
  the process-exit machinery, not by `whereis`.
- Making `whereis` probe liveness would (a) change semantics, (b)
  couple `orc` to `proc` internals, and (c) add a per-lookup cost on
  the hot path. **Out of scope; do not add.**

`ponytail: whereis returns the registered pid without a liveness
probe -- same as today. Stale-entry cleanup belongs to whoever owns
the named proc's exit path (a monitor or supervisor unregistering on
DOWN), not to the lookup. If automatic cleanup is ever wanted, add an
xtc_monitor in the registrar and call xtc_reg_unregister from the DOWN
handler -- a registry change is not needed.`

One concurrency nuance worth a comment under Option B: a lookup may
return a pid for a name being unregistered concurrently (reader saw
the node before the writer's unlink). This is inherent and harmless --
the caller's subsequent `xtc_send` validates the pid's generation and
fails cleanly if the proc is gone. The registry's contract is
"name -> last-published pid", not "name -> currently-live pid".

## 9. Test plan

Existing `test/m10/test_reg.c` must pass unchanged -- it is the
regression floor (duplicate rejection, whereis hit/miss, unregister,
count, re-register). Add cases to the same file (it already links
`munit` and `xtc_reg.h`; `dist/Makefile.in:987-991`), so no new
Makefile target is required:

1. **Collision test.** Pick names that collide modulo
   `REG_NBUCKETS=256`. Either brute-force in the test (loop generating
   `"k%d"` strings, compute the same FNV-1a as `reg.c`, keep two whose
   `hash & 255` match) or hardcode a known colliding pair found
   offline. Register both, assert both `whereis` correctly (distinct
   pids), unregister the *first-inserted* (chain tail vs head both
   exercised), assert the other still resolves. This proves chain
   walking and unlink-by-link are correct.

2. **Scale / O(1) test.** Register N names (`N = 5000`,
   `"svc-%d"`), assert `xtc_reg_count == N`, then `whereis` every
   name and assert each pid round-trips. Optionally time a fixed
   number of lookups at N=100 vs N=5000 and assert the ratio stays
   bounded (well under linear) -- but keep any timing assertion loose
   (e.g. ratio < 4x) to avoid CI flakiness under sanitizers. The
   primary assertion is correctness at scale; the timing is
   informational. This is the "proves O(1)" case; with 256 buckets
   the 5000-name average chain is ~20, still flat per-lookup versus
   the old 5000-element linear scan.

3. **Concurrent lookup during register/unregister (Option B only).**
   Model on `test/m13/test_rcu.c` (it already shows the
   reader-thread + `xtc_rcu_synchronize`-on-a-helper-thread pattern,
   `test_rcu.c:60-117`). Spawn K reader threads that loop
   `xtc_reg_whereis` on a fixed set of names while the main thread
   churns `register`/`unregister` of *other* names. Assert: (a) no
   crash / no UAF (run under ASan + UBSan -- the CI sanitizer jobs in
   AGENTS.md will exercise this), (b) every lookup of a
   never-unregistered name always succeeds with the right pid, (c)
   after `join`, `xtc_rcu_synchronize` x3 then assert retired nodes
   were freed (reuse the `g_freed_count` free-counter idiom from
   `test_rcu.c:18-25` by giving the test a custom node-free shim, or
   simply run under ASan and rely on leak detection).
   Call `xtc_rcu_init()` at test start and `xtc_rcu_fini()` at end
   (`test_rcu.c:32,54`).

4. **Destroy-with-pending-retire (Option B only).** Register a name,
   unregister it (node now retired, not yet reclaimed), immediately
   `xtc_reg_destroy`. Assert no crash and -- with `xtc_rcu_fini` at
   teardown -- no leak. Guards the section 5.3 subtlety.

For Option A, only tests 1 and 2 apply (no RCU). Both options keep
test 0 = the unmodified original.

## 10. Performance expectation

- `whereis`: O(n) -> O(1) average (chain length ~ load factor).
  Option B additionally removes mutex acquire/release from the read
  path entirely -- the win that matters when many loop threads resolve
  names concurrently. The read fast path becomes two atomic loads +
  `strcmp`s along a short chain, no syscall, no CAS
  (`xtc_rcu_read_lock` is a TLS store, `xtc_rcu.h:60-61`).
- `register`/`unregister`: O(n) -> O(1) average for the lookup
  portion; both still serialize on the writer mutex (fine -- rare).
  Under B, `unregister` adds a cheap `xtc_rcu_retire` (a slab alloc +
  list push, `rcu.c:198-227`); it does **not** block on a grace
  period.
- Memory: one extra `struct reg_node` per entry (~32 bytes: a
  pointer, a `uint32_t`, an 8-byte pid, a name pointer) plus a fixed
  `256 * sizeof(void*)` = 2 KiB bucket array per registry. Negligible.

## 11. Risk assessment (honest)

- **Option A risks: essentially none.** It is the `alloc_audit.c`
  pattern applied to a string key. The only bug surface is the FNV
  string variant and the unlink-by-link idiom, both copied from
  proven code. Strongly recommend landing A regardless.
- **Option B risks, ranked:**
  1. *Memory-ordering bugs* on the publish/consume of `buckets[]` and
     `next`. Mitigation: strict acquire/release as specified;
     validate under TSan/UBSan (the CI jobs in AGENTS.md). This is the
     highest risk and the reason to stage B behind A.
  2. *Reclamation lag / destroy interaction* (section 5.3). A
     mis-handled destroy either leaks retired nodes or double-frees.
     Mitigation: destroy frees only still-linked nodes; retired nodes
     are RCU-owned. Test 4 guards it.
  3. *RCU lifecycle coupling.* If the embedding app never advances the
     epoch (no writer ever calls `synchronize`, no periodic helper),
     retired nodes accumulate until `xtc_rcu_fini`. For a registry
     this is bounded by total unregister count and is harmless in
     practice, but document it. The `PLAN.md` note about a "periodic
     helper" advancing the epoch (`xtc_rcu.h:16-18`) is the proper
     long-term answer.
  4. *`xtc_rcu` is itself M13-era and single-global-epoch*
     (`rcu.c:9-13`); its `synchronize` `sched_yield`-spins waiting for
     readers (`rcu.c:226-233`). A pathological reader that holds a
     read-side forever would stall a writer's `synchronize` -- but the
     registry's read sections are a few-instruction `strcmp` walk, so
     this cannot happen from registry reads.
- **Non-risk explicitly noted:** no over-alignment, so no
  `__os_aligned_alloc` correctness trap (the AGENTS.md alignment rule
  does not bite here).

## 12. Recommendation summary

1. Land **Option A** (hashed buckets, keep mutex) as the correctness
   floor -- mechanical, matches `alloc_audit.c`, zero RCU risk. Tests
   0-2.
2. Follow with **Option B** (RCU lockless reads) to deliver the
   wait-free reads the original comment promised -- same layout,
   add atomic publish + retire/grace-period. Tests 0-4 under
   sanitizers.
3. Do **not** build a general `xtc_chash` for this; embed the small
   chained table in `reg.c`. If `xtc_chash` is later built for M13a,
   `reg.c` can adopt it then -- but that is a separate, larger effort
   and not justified by the registry's needs alone.
4. Do **not** add pid-liveness validation to `whereis`; staleness is
   the consumer/send-boundary's concern, as it already is.

### Critical Files for Implementation
- /home/gburd/ws/xtc/src/orc/reg.c - The only source file that changes; `struct xtc_reg` and all five function bodies are rewritten around the chained table (currently the linear table at lines 21-133).
- /home/gburd/ws/xtc/src/ptc/lock_mgr.c - FNV-1a constants (`:166-172`) and the fixed-array/chained-bucket + unlink-by-link conventions to copy verbatim.
- /home/gburd/ws/xtc/src/ptc/alloc_audit.c - The closest template: power-of-two masked buckets, head-insert, `**link` remove, teardown loop, single-mutex model (Option A).
- /home/gburd/ws/xtc/src/ptc/rcu.c (and src/inc/xtc_rcu.h) - The RCU read_lock/retire/synchronize API and its two-epoch reclamation lag that Option B depends on.
- /home/gburd/ws/xtc/test/m10/test_reg.c - The regression floor and where the new collision / scale / concurrency tests are added (build wired at dist/Makefile.in:987-991).
