# M_DISTRIBUTED.md -- Distributed spawn / link / monitor for libxtc (DESIGN, not implemented)

Status: DESIGN PROPOSAL.  Nothing here is built.  This document reviews
how Erlang/OTP supports spawn, link, monitor, and message passing over
network connections, then proposes an analogous feature set for libxtc,
assesses feasibility, and recommends how to package it (in-tree option,
separate library, or core).

--------------------------------------------------------------------------------
## 1. How Erlang/OTP does distribution

Erlang distribution is the reason `spawn`, `!` (send), `link`, and
`monitor` work transparently whether the peer is a fiber in the same VM
or a process on a machine across the network.  The mechanism, layer by
layer:

### 1.1 Node identity and naming
- Every VM instance is a NODE with a name like `worker@host.example.com`.
  A node name is globally unique within a cluster.
- A PID is `<node, id, serial, creation>`.  Locally it prints `<0.123.0>`;
  the `0` is the node index in the local node table (0 == this node).  A
  remote pid carries the originating node so the runtime knows where the
  real process lives.
- `creation` is an incarnation counter: if a node restarts, its creation
  changes, so a stale pid minted by the previous incarnation is detected
  and rejected (the distributed analogue of libxtc's per-slot `gen`).

### 1.2 The Erlang Port Mapper Daemon (epmd)
- A small daemon per host.  Each starting node registers `(name -> TCP
  port)` with epmd.  To reach `worker@host`, a node asks that host's epmd
  "what port is `worker` on?" then dials it.  epmd is just a rendezvous /
  name-to-port directory; it carries no traffic.

### 1.3 The distribution handshake and transport
- Node A dials node B's port.  A version + capability handshake runs,
  then a COOKIE-based authentication: both nodes must share a secret
  atom (the "magic cookie"); a challenge/response proves it.  This is
  authentication, not encryption -- classic Erlang distribution is
  cleartext unless run over TLS distribution (`inet_tls_dist`).
- Once up, the connection is a single long-lived TCP stream carrying a
  framed, tagged protocol.  All traffic between the two nodes (every
  send, link, monitor, exit, spawn request) multiplexes over this one
  connection.  Connections are established lazily on first contact and
  are transitive-by-default: talking to a node teaches you about the
  nodes it knows (`global` and `net_kernel` maintain the mesh).

### 1.4 Control messages (the distribution protocol)
Over the connection flow TAGGED control tuples, each optionally followed
by a payload term.  The important ones:
- `SEND` / `REG_SEND`   -- deliver a message to a pid (or a registered
                           name) on the remote node.
- `LINK` / `UNLINK`     -- establish/break a bidirectional link across
                           the connection.
- `MONITOR_P` / `DEMONITOR_P` -- set/clear a monitor.
- `EXIT` / `EXIT2`      -- propagate an exit signal to a linked remote
                           process.
- `MONITOR_P_EXIT`      -- deliver a DOWN to a remote monitor.
- `SPAWN_REQUEST` / `SPAWN_REPLY` (OTP 23+) -- ask the remote node to
  spawn a process running `{Module, Fun, Args}` and reply with its pid;
  the pre-23 idiom was `spawn(Node, M, F, A)` implemented as a REG_SEND
  to the remote `rex`/`erpc` server.  `spawn_request` supports an atomic
  link/monitor option so the relationship exists before the child runs
  -- exactly the local `spawn_link` guarantee, extended over the wire.

### 1.5 Failure model that makes it usable
- A distributed link/monitor fires on THREE things, indistinguishably to
  the application except by reason: the remote process exited, the remote
  process crashed, or THE CONNECTION WENT DOWN (node unreachable).  A
  node-down delivers `noconnection` as the exit/DOWN reason.  This is the
  crucial property: a network partition looks like a peer death, so
  supervision trees that already handle death handle partition for free.
- `net_kernel` runs a heartbeat (net ticks) over each connection; if a
  peer misses N ticks, the connection is declared down and every link/
  monitor spanning it fires `noconnection`.  A partition is thus turned
  into ordinary DOWN/EXIT signals after a bounded detection time.
- Ordering guarantee: messages between the SAME pair of processes are
  delivered in send order (per-connection FIFO), but there is NO global
  ordering and NO delivery guarantee across a partition (a message sent
  into a broken connection is lost, and the sender learns via the link/
  monitor firing, not via a send error).

### 1.6 What Erlang deliberately does NOT promise
- Not exactly-once delivery -- at-most-once; the app rebuilds reliability
  from links/monitors + retries.
- Not location-transparent PERFORMANCE -- a remote send is orders of
  magnitude slower; the transparency is semantic, not cost.
- Not secure by default -- cookie auth is trivially forgeable on an open
  network; TLS distribution or a private network is required for real
  security.
- Not partition-tolerant CONSISTENCY -- distribution is AP-ish; `global`
  name registration can split-brain under partition.

--------------------------------------------------------------------------------
## 2. Proposed libxtc feature set: "xtcdist"

The design mirrors Erlang's layering onto libxtc's existing substrate.
libxtc ALREADY has every local ingredient: directed pids
(loop_id/local_id/gen -- gen is the local incarnation counter, the
analogue of Erlang's per-pid serial), link/monitor with EXIT ('E') and
DOWN ('D') signals (src/ptc/proc.c __notify_links_and_monitors), a
length-framed TCP/UNIX transport (xtc_net_send_frame /
xtc_net_recv_frame), a name registry (xtc_reg), and gen_server RPC
(xtc_svr).  Distribution is the disciplined extension of these across a
connection.

### 2.1 Node identity: xtc_node_t
Add a node dimension to the pid.  Two options:

  (A) Widen xtc_pid_t with a `node_id` (uint16_t) -- an index into a
      per-node connection table, 0 == local.  This is Erlang's exact
      model (node 0 == self).  It changes the pid ABI (a breaking change:
      xtc_pid_t grows from 8 to 10/12 bytes), so it must be gated to a
      major version or an opt-in build.

  (B) Keep xtc_pid_t unchanged for local use and introduce a SEPARATE
      xtc_gpid_t ("global pid") = { xtc_node_t node; xtc_pid_t local; }
      used only by the distributed API.  Local code is untouched; only
      code that goes distributed uses the wider handle.

  RECOMMENDATION: (B).  It keeps the hot local path and the stable
  local ABI byte-identical, and makes "this is a remote reference"
  explicit in the type -- which is honest, because a remote send is not
  cost-transparent.  The distributed layer translates xtc_gpid_t <->
  xtc_pid_t at the connection boundary.

  A node is identified by a stable name string (e.g. "worker@10.0.0.3")
  plus a `creation` incarnation counter minted at node start (bumped on
  restart, so a pid from a dead incarnation is rejected -- the
  distributed analogue of the local `gen`).

### 2.2 Rendezvous: how a node finds a peer's port
Two supported modes, no daemon required by default:
  - STATIC: the embedder passes a table of `name -> host:port` (config
    file / API).  Simplest; fits a PostgreSQL-style fixed cluster where
    the node set is known.  RECOMMENDED default.
  - DIRECTORY (optional): a tiny epmd-analogue, `xtc_nodemapd`, that maps
    name -> port on a host.  Only needed for dynamic ports.  Kept
    OPTIONAL because most deployments have fixed ports.

### 2.3 Connection + handshake: xtc_node_connect
  int xtc_node_connect(xtc_dist_t *d, const char *node_name,
                       const char *host, int port);
Establishes (lazily, on first cross-node reference, or eagerly via this
call) a single long-lived framed connection per peer node, owned by a
dedicated CONNECTION PROC (an xtc_proc per peer) that:
  - performs a version+capability handshake,
  - authenticates with a shared-secret challenge/response (a cookie), and
  - optionally wraps the socket in xtc_tls (libxtc already has a TLS
    layer) -- TLS is the RECOMMENDED transport for any non-loopback link.
The connection proc owns the socket; all traffic to that peer is sent by
handing framed control messages to this proc's mailbox, so the socket has
a single writer (no cross-thread socket races), and inbound frames are
demultiplexed by this proc into local deliveries.

### 2.4 The wire protocol: framed, tagged control messages
Reuse xtc_net_send_frame / recv_frame (4-byte length + payload).  Each
frame is `{ uint8 tag; ... }`.  The tag set mirrors Erlang's:
  DIST_SEND        { dst_local_pid, msg_bytes }
  DIST_REG_SEND    { dst_name, msg_bytes }         -- send to a registered name
  DIST_LINK        { from_gpid, to_local_pid }
  DIST_UNLINK      { from_gpid, to_local_pid }
  DIST_MONITOR     { ref, watcher_gpid, target_local_pid }
  DIST_DEMONITOR   { ref }
  DIST_EXIT        { to_local_pid, reason, exit_kind }  -- link EXIT across
  DIST_DOWN        { ref, target_gpid, reason, exit_kind } -- monitor DOWN
  DIST_SPAWN_REQ   { req_id, fn_id, arg_bytes, rel(none|link|monitor) }
  DIST_SPAWN_REP   { req_id, child_gpid, rc }
  DIST_TICK        {}                               -- heartbeat
Note DIST_EXIT/DIST_DOWN carry the v1.3.0 exit_kind byte + reason so the
self-describing DOWN classification (clean/exit/signal/noproc) survives
the wire unchanged -- a remote crash decodes as KIND_SIGNAL, a remote
clean exit as KIND_CLEAN, and a lost connection as a NEW kind
XTC_DOWN_KIND_NOCONNECTION (Erlang's `noconnection`).

Because a remote fn cannot be a raw function pointer (address space
differs), DIST_SPAWN_REQ names the function by a REGISTERED id: the
embedder registers `xtc_dist_register_fn("worker_main", worker_main)` on
each node at startup, and spawn_request carries the string/id, resolved
to the local fn pointer on the target.  This is Erlang's {Module, Fun,
Args} by-name model, adapted to C.

### 2.5 The distributed API (the user-facing surface)
Deliberately parallel to the local API so the mental model transfers:
  int xtc_dist_spawn        (xtc_dist_t *d, const char *node,
                             const char *fn_name, const void *arg,
                             size_t arglen, xtc_gpid_t *out);
  int xtc_dist_spawn_link   (... same ..., xtc_gpid_t *out);      /* atomic */
  int xtc_dist_spawn_monitor(... same ..., xtc_gpid_t *out, uint64_t *ref);
  int xtc_dist_send         (xtc_gpid_t dst, const void *msg, size_t len);
  int xtc_dist_send_name    (const char *node, const char *name,
                             const void *msg, size_t len);
  int xtc_dist_link         (xtc_gpid_t peer);
  int xtc_dist_monitor      (xtc_gpid_t target, uint64_t *ref);
A local proc receives a remote message / EXIT / DOWN through its ORDINARY
mailbox (xtc_recv) using the SAME envelope shapes as local ones, plus the
new NOCONNECTION reason.  So an existing supervisor that classifies DOWN
with xtc_down_decode_ex handles a remote crash, a remote exit, AND a
node partition with zero new code -- exactly Erlang's payoff.

### 2.6 Partition = death (the key semantic)
The connection proc runs a heartbeat: it sends DIST_TICK every T and
expects to see peer traffic within N*T.  On miss (or socket error), it
declares the peer DOWN and, for every link/monitor spanning that
connection, injects a local EXIT/DOWN with reason
XTC_DOWN_KIND_NOCONNECTION.  A healed connection is re-established lazily
on the next reference (new incarnation if the peer restarted, detected
via creation).  This turns an unbounded network hang into a bounded,
observable, supervision-tree-handled event.

--------------------------------------------------------------------------------
## 3. What it offers users

- TRUE location transparency of SEMANTICS: spawn a worker on another node,
  link/monitor it, and get an EXIT/DOWN whether it crashes, exits, or its
  node vanishes -- with the same code that handles a local child.  This is
  the single most valuable property Erlang gives distributed systems.
- A supervision tree that already restarts local children extends to
  cross-node children with no new failure logic (partition folds into
  DOWN).
- A natural clustering story for the things libxtc's examples model: a
  multi-node kaka (Kafka) broker cluster, a sharded rexis (Redis) with
  cross-node key ownership, or -- the strategic target -- a threaded
  PostgreSQL scaled across MACHINES, not just loops, with backends
  monitored across the cluster the same way they are monitored across
  loops today.
- Reuse of the entire existing local programming model: no second API to
  learn, just the `_dist_` prefix and the wider xtc_gpid_t handle.

--------------------------------------------------------------------------------
## 4. Feasibility, and deterministic testing

### 4.1 Implementation feasibility: MEDIUM-HIGH
The substrate exists (pids, link/monitor, framed transport, TLS, registry,
gen_server), so this is mostly PLUMBING + a protocol, not new runtime
mechanism.  The genuinely new pieces are:
  - the connection proc + handshake + heartbeat (a well-understood state
    machine),
  - the control-message codec (straightforward framed tag/payload),
  - the gpid <-> local-pid translation table per connection,
  - the by-name function registry for remote spawn,
  - the NOCONNECTION reason plumbed through the existing notify path.
None of these require touching the hot local scheduler.  Estimated size:
one new src/dist/ subtree (~2500-4000 LOC) + a handful of new public
headers.  The hard parts are not novel: framing/backpressure on a slow
peer, half-open connection detection, and the incarnation/creation
handshake -- all textbook, all covered by the design above.

### 4.2 Deterministic testing: FEASIBLE, and this is the strong argument
This is where libxtc's DST investment pays off enormously.  A distributed
protocol is EXACTLY what DST is best at, and FoundationDB (a distributed
database) is the proof that a networked, partition-prone, failure-ridden
distributed protocol can be tested to "it works flawlessly" confidence in
a single-threaded deterministic simulator.  Concretely:

  - Model each node as an xtc_exec (an N-loop executor) INSIDE ONE sim
    process, exactly as the existing multi-loop sim already models
    "machines".  A "cluster" is several xtc_execs driven by one seeded
    scheduler.  This is precisely FDB's Sim2: all nodes in one process,
    one deterministic clock.
  - The inter-node CONNECTION is NOT a real socket under sim; it is the
    existing cross-loop message path (proc.c __mbox_deliver +
    io_sim.c deferred delivery), which ALREADY models seeded latency,
    reorder (the swizzle lever added in the FDB-parity work), and
    DROP via the partition matrix.  So the distribution protocol runs
    over the SAME simulated network the local cross-loop tests already
    use -- no new transport simulation is needed.
  - Partition testing is FREE: the existing symmetric AND asymmetric
    partition matrix (test_sim_partition) cuts a connection; the design's
    "partition => NOCONNECTION DOWN" behavior becomes a DST assertion:
    "cut node A<->B, assert every cross-node link/monitor fires
    NOCONNECTION within the heartbeat bound, and the run quiesces and
    replays."
  - The self-describing DOWN + atomic spawn_link/monitor (already built,
    v1.3.0) give the exact invariants to assert across the wire: a remote
    spawn_monitor of an instant-exiting child delivers a real-reason DOWN
    (never NOPROC), a remote crash delivers KIND_SIGNAL, a partition
    delivers KIND_NOCONNECTION -- all checkable deterministically.
  - The consistency-check seam (also just built) lets an end-of-run check
    assert a GLOBAL cluster invariant (e.g. "every acked cross-node write
    is present on the owning node after a partition heals").
  - Heartbeat/timeout logic is tested against the DST virtual clock; the
    planned clock-skew lever (DST wave 2) directly stresses the tick
    detector.
  - Node REBOOT (the planned DST wave-3 lever) models an incarnation
    change: kill a node's execs, restart with a bumped creation, assert
    stale gpids are rejected and the mesh re-forms.

  The claim "proves it functions flawlessly" is, honestly, the same claim
  FDB makes and backs: a large seeded sweep over {partition x latency x
  reorder x node-death x reboot x clock-skew} with per-run invariant +
  end-of-run consistency checks and byte-identical replay.  It is not a
  mathematical proof, but it is the strongest empirical assurance the
  industry has for distributed protocols, and libxtc's DST is already
  built to deliver it.  The REAL kernel bits that cannot be simulated
  (an actual TCP socket handshake, real TLS bytes on the wire) are tested
  separately with a small real-socket integration test on loopback and
  across the existing multi-platform SSH hosts -- the same "decline the
  genuine kernel bit, simulate the protocol" split the osproc/blocking
  DST work already established.

### 4.3 What CANNOT be proven by DST (be honest)
- Real wire security (a forged cookie, a MITM without TLS) -- a design/
  audit concern, not a DST one.
- Real-network pathologies beyond the model (NIC bugs, middlebox
  rewriting, real TCP incast collapse) -- partially reachable by widening
  the fault model, never fully.
- Performance/throughput at scale -- a benchmark concern, not correctness.

--------------------------------------------------------------------------------
## 4.4 What the tnt / Tina layer teaches this design

libxtc's tnt layer (the in-tree Tina-style stackless Isolate layer,
src/orc/tnt.c + xtc_tnt.h) does NOT itself support distribution -- it is
by design a single-node, thread-per-core, shared-nothing Isolate
scheduler, and Tina upstream (github.com/pmbanugo/tina) is likewise
single-node.  So there is no distributed spawn/link to copy from it.  But
two properties of tnt/Tina directly inform and reinforce this design:

  (1) The tnt HANDLE ALREADY CARRIES A SHARD DIMENSION.  A tnt handle is
      a 64-bit generational id laid out as
        | shard_id (8) | type_id | slot_index | generation |
      and xtc_tnt_send routes a message by shard_id across shards, with a
      stale-handle check via the generation counter (a send to a torn-
      down Isolate returns XTC_TNT_SEND_STALE_HANDLE).  This is EXACTLY
      the structural precedent for the proposed xtc_gpid_t: a node_id is
      to a global pid what tnt's shard_id is to a handle, and tnt's
      generation is what a node's creation incarnation counter is.  The
      design's node dimension is therefore not a foreign concept bolted
      on -- it is the same "prepend a locality index + keep a generation
      for staleness" pattern tnt (and Erlang, and the local xtc_pid gen
      field) already use.  A future xtc_tnt_send that accepts a handle
      whose shard_id denotes a REMOTE node's shard is a natural
      extension: the shard router hands a cross-node handle to the
      distribution layer's connection proc instead of a local shard
      mailbox, and the same stale/generation check becomes the
      cross-node incarnation check.

  (2) Tina is DETERMINISTIC-SIMULATION-FIRST by design ("100%
      Deterministic Simulation... TigerBeetle-style: same seed + same
      config = same execution").  Independent corroboration of section
      4.2's central claim: a message-passing actor layer is built to be
      DST-tested, and extending it across nodes does not break that as
      long as the inter-node link is modelled as the existing simulated
      cross-loop/cross-shard message path.  tnt's effect-interpreter
      model (an Isolate returns an EFFECT the scheduler commits) is, in
      fact, EASIER to distribute-and-simulate than stackful fibers,
      because the effect is already a reified, serialisable description
      of what the Isolate wants to do -- a cross-node effect (send to a
      remote handle) is just another effect tag the interpreter routes
      to the connection proc.

INTEGRATION RECOMMENDATION for tnt: if the distributed module is built,
expose distribution to tnt by WIDENING THE SHARD ROUTER, not by adding a
parallel API.  A remote Isolate is addressed by a handle whose shard_id
maps (via a per-node shard table) to a remote node; xtc_tnt_send to such
a handle serialises the message and hands it to the distribution
connection proc; an inbound cross-node message is injected into the
target shard exactly as a local cross-shard send.  The tnt effect
interpreter gains one new effect route (remote send / remote spawn) and
the existing stale-handle path doubles as the cross-node incarnation
check.  This keeps tnt's single, uniform send/handle model intact and
makes distribution transparent to Isolate authors -- the same payoff the
core xtc_dist API gives raw-proc authors.  It belongs in the same
--enable-dist module, gated so a tnt build without distribution is
byte-unchanged.

--------------------------------------------------------------------------------
## 5. Packaging recommendation

Three options were considered:

  (a) CORE (in libxtc, always built).  Rejected: it drags a network
      protocol, a connection state machine, and a security surface into
      every build, including the embedded/single-node PostgreSQL-backend
      use that is libxtc's primary target and wants a minimal, auditable
      core.  Distribution is not needed by most consumers.

  (b) OPTIONAL IN-TREE MODULE (built into libxtc behind a configure flag
      --enable-dist / a meson option, default OFF; its own src/dist/
      subtree and xtc_dist.h header, excluded from the amalgamation
      unless enabled).  This is how libxtc already gates TLS backends and
      io_uring.  The distributed code links against the core; the core
      never depends on it.

  (c) SEPARATE LIBRARY (libxtcdist, its own repo/package, depends on
      libxtc's public headers).  Cleanest separation, but adds a
      release/versioning axis and forces the distributed layer to use
      only the PUBLIC libxtc API (it currently would want a couple of
      internal seams -- e.g. injecting a NOCONNECTION EXIT into a local
      proc's notify path -- which would have to be promoted to public,
      slightly widening the stable surface).

  RECOMMENDATION: (b), OPTIONAL IN-TREE MODULE, with a clean internal
  boundary, and design it so it COULD later be extracted to (c) if it
  grows a large independent surface.  Rationale:
   - It keeps the core minimal and auditable for the single-node
     consumers (the default build is byte-unchanged).
   - It lets the distributed layer use the few internal seams it needs
     (notify-path injection, the sim cross-loop transport) without
     prematurely widening the public ABI -- while those seams are
     exercised by the SAME sim harness, so they stay honest.
   - It ships and versions with the core, so the atomic spawn_link /
     self-describing DOWN / DST guarantees the distributed layer depends
     on are always in lockstep.
   - The moment the distributed layer has a stable, self-contained public
     API and no longer needs internal seams, extraction to a separate
     library is a mechanical move -- the module boundary is designed to
     make that cheap, not to preclude it.

  The primary consumer (threaded PostgreSQL) reinforces (b): a single-node
  PG backend build wants NO distribution code, but a future multi-node PG
  wants it in lockstep with the core's process semantics -- an optional
  in-tree module gives both from one source tree and one release.

--------------------------------------------------------------------------------
## 6. Summary

- Erlang distribution = node identity + a rendezvous + one authenticated
  long-lived framed connection per peer + tagged control messages for
  send/link/monitor/exit/spawn + a heartbeat that turns partition into
  ordinary DOWN/EXIT signals.
- libxtc already has every local ingredient; distribution is the
  disciplined extension of them across a connection, plus a by-name
  remote-spawn registry and a NOCONNECTION reason.
- The user payoff is Erlang's: one supervision model that handles local
  crash, remote crash, and network partition uniformly.
- Feasibility is MEDIUM-HIGH (mostly plumbing over existing primitives),
  and -- crucially -- it is DETERMINISTICALLY TESTABLE to FDB-grade
  confidence because the sim already models a multi-node cluster with a
  seeded, partition-capable, reorder-capable, latency-capable network;
  the distributed protocol runs over that same simulated network.
- Package it as an OPTIONAL IN-TREE MODULE (--enable-dist, default OFF),
  designed with a clean boundary so it can be extracted to a separate
  library later if warranted.  It is NOT core (keep the single-node build
  minimal) and NOT yet a separate library (it needs a few internal seams
  and must version in lockstep with the process-semantics guarantees it
  builds on).
