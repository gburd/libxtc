# tnt -- a Tina-faithful Isolate layer on libxtc

`tnt` is a thread-per-core, shared-nothing **Isolate** layer built on
top of the libxtc concurrency runtime.  You write simple, synchronous
state machines (Isolates) that react to messages and return
**transitions**; the layer's scheduler runs them, batches them by type
in dense arenas, references them by generational handle, and commits
their I/O effects through libxtc's reactor.

It reproduces the developer experience of [Tina](https://github.com/pmbanugo/tina)
(Odin) on libxtc's machinery.  The design and feasibility verdict are
in [`docs/M_TINA_LAYER.md`](../../docs/M_TINA_LAYER.md), which is
authoritative.

This is `examples/08_tnt/` -- a self-contained consumer of libxtc's
**public API only**.  It does not modify libxtc core.

## The canonical example: a TCP echo server

Mirrors Tina's README echo.  Each connection is its own Isolate; no
shared socket table, no lock.

```c
typedef struct echo_conn {
	int     fd;
	uint8_t buffer[256];
	int     bytes;
} echo_conn_t;

/* Initialize: record the fd and submit the first recv. */
static tnt_transition_t
echo_init(void *self_raw, const void *args, size_t args_size)
{
	echo_conn_t *self = tnt_self_as(echo_conn_t, self_raw);
	const echo_args_t *a = args;
	self->fd = a->client_fd;
	if (tnt_submit_recv(self->fd) != TNT_IO_OK)
		return tnt_transition_to_crash(TNT_FAULT_CONTRACT_VIOLATION);
	return TNT_TRANSITION_WAIT_IO;
}

/* Handle I/O completions synchronously.  If anything fails: let it crash. */
static tnt_transition_t
echo_handler(void *self_raw, tnt_message_t *msg)
{
	echo_conn_t *self = tnt_self_as(echo_conn_t, self_raw);
	switch (msg->tag) {
	case TNT_IO_TAG_RECV_COMPLETE:
		if (msg->body.io.result <= 0) {
			(void)tnt_submit_close(self->fd);
			return TNT_TRANSITION_WAIT_IO;
		}
		self->bytes = msg->body.io.result;
		memcpy(self->buffer, msg->body.io.buffer, self->bytes);
		(void)tnt_io_send(self->fd, self->buffer, self->bytes);
		return TNT_TRANSITION_WAIT_IO;
	case TNT_IO_TAG_SEND_COMPLETE:
		(void)tnt_submit_recv(self->fd);
		return TNT_TRANSITION_WAIT_IO;
	case TNT_IO_TAG_CLOSE_COMPLETE:
		return TNT_TRANSITION_DONE;
	default:
		return TNT_TRANSITION_WAIT_MESSAGE;
	}
}
```

A plain listener proc accepts connections and spawns one EchoConnection
Isolate per connection (round-robin across shards) via `tnt_spawn_on`.

## Architecture

The decisive choice (per `M_TINA_LAYER.md`) is to map a **Shard to one
long-lived libxtc proc** per `xtc_exec` loop -- NOT an Isolate to a
proc.

```
+--------------------------------------------------+
|                  SHARD (one xtc_proc)            |
|                                                  |
|  Typed arenas (one per Isolate type)             |
|   [Echo][Echo][Echo]...   [Timer][Timer]...      |
|   slab-carved at boot, generational slots        |
|                                                  |
|  Dispatch loop (the shard fiber):                |
|    drain inbox                                   |
|    -> collect reactor completions                |
|    -> for each ready type, for each ready slot   |
|       (budgeted): handler -> interpret transition|
|                                                  |
|  Turn-scratch bump arena (reset per handler)     |
|  Completion ring (fed by I/O courier fibers)     |
|  Self-wake pipe                                  |
+--------------------------------------------------+
```

Handlers **never block**.  Actions (send, spawn, submit I/O, arm a
timer) are staged via ambient `tnt_*` calls during the handler; the
returned transition tells the shard how to commit them.  Only the shard
is a fiber; Isolates are stackless arena structs.

### How I/O works on libxtc

When a handler stages `tnt_submit_recv` / `tnt_io_send` /
`tnt_submit_close` and returns `TNT_WAIT_IO`, the shard commits each
staged op by spawning a short-lived **courier** fiber.  The courier
parks on the fd (`xtc_proc_wait_fd`), performs the non-blocking syscall,
posts a completion record to the shard's ring, and wakes the shard.  The
courier is the shard's I/O mechanism -- not an Isolate.  The Isolate
stays a stackless arena struct; only the courier (bounded by the number
of outstanding ops) carries a stack.  This uses purely public libxtc
APIs (`xtc_proc_*`, `xtc_io_*`, `xtc_net_*`).

## What maps from Tina

| Tina concept | tnt |
|---|---|
| `handler(self, msg) -> Effect` | `tnt_handler_fn` returning `tnt_transition_t` |
| 64-bit generational handle (shard:type:slot:gen 8:8:20:28) | `tnt_handle_t`, same bit layout |
| stage-then-commit turn frame (`ctx_*`) | ambient `tnt_*` calls valid only during a handler |
| transition kinds DONE/YIELD/WAIT_MESSAGE/WAIT_IO/CRASH | `tnt_transition_kind_t` |
| bounded mailbox, drop-on-full with sender feedback | `tnt_send` returns `TNT_SEND_MAILBOX_FULL` |
| typed dense arenas, batched dispatch by type | per-type arena + budgeted per-type scan |
| spawn returns a handle synchronously, init runs now | `tnt_spawn` / `tnt_spawn_on` |
| one-shot timers delivering a tagged message | `tnt_register_timer` |
| thread-per-core, shared-nothing shards | one shard proc per `xtc_exec` loop |

## What is honestly NOT done here (the asterisks)

Carried straight from `M_TINA_LAYER.md`:

1. **DST (deterministic simulation testing) is layer-built, not done
   here.** The transition model keeps the "handlers only return
   transitions, never block" invariant, so a scripted effect
   interpreter can be swapped in for DST later -- but this slice ships
   only the production interpreter.

2. **No-malloc-after-boot is DISCIPLINARY, not structural.** Arenas
   (slot metadata + the Isolate-struct store) are carved from boot-time
   `xtc_slab` caches.  But the hot path still mallocs a message
   envelope per send and a recv buffer per recv (via `__os_malloc`), and
   couriers/timers are spawned with allocation.  Tightening these onto
   slab pools is straightforward follow-on work; the structural claim
   ("after boot, malloc is never called") is not yet enforced.

3. **Cross-shard messaging uses the in-process libxtc transport.** All
   shards share the process address space, so `tnt_send` to another
   shard delivers directly into that shard's arena mailbox (waking it
   via its pipe).  Tina's faithful per-shard-pair lock-free SPSC rings
   are not built here; this is the "approximate" transport from the
   doc.

4. **Supervision is Level-1 only.** A handler returning `TNT_CRASH` or
   `TNT_DONE` tears down its own arena slot (bumping the generation).  A
   real SIGSEGV would unwind the shard fiber (Tina's Level-2 shard
   rebuild) -- libxtc has the trap (`xtc_proc_recovery_arm`) for it, but
   wiring shard rebuild + restart strategies is follow-on work.

## Build and run

libxtc must be built first:

```sh
cd ../../build_unix && make
```

Then, from this directory:

```sh
make              # build libtnt.a + tnt_echo + test_tnt
make test         # run the in-process dispatch test (no network)
make echo-test    # run the end-to-end TCP echo round-trip test
make clean
```

The in-process test (`test_tnt`) proves, on the real shard scheduler:
spawn into a typed arena, the dispatch loop, WAIT_MESSAGE / YIELD / DONE
transitions, drop-on-full bounded mailboxes with sender feedback,
generational-handle staleness after teardown, and one-shot timers.

The echo test (`test_echo.sh`) starts `tnt_echo` on a port, opens TCP
connections (single round-trips plus 8 concurrent), and verifies each
payload is echoed back byte-for-byte across two shards.

### Running the echo server by hand

```sh
./tnt_echo 7777 2        # port 7777, 2 shards
# in another shell:
printf 'hello' | nc 127.0.0.1 7777
```

## Public API

The entire surface is in `tnt.h`:

- Types: `tnt_handle_t`, `tnt_transition_t`, `tnt_message_t`,
  `tnt_type_t`, `tnt_spec_t`.
- Boot: `tnt_start`, `tnt_stop`, `tnt_spawn_on`.
- Ambient (handler-only): `tnt_send`, `tnt_spawn`, `tnt_submit_recv`,
  `tnt_io_send`, `tnt_submit_close`, `tnt_register_timer`, `tnt_self`,
  `tnt_shard_id`, `tnt_scratch_arena`.
- Helpers: `tnt_self_as`, `tnt_payload_as`,
  `tnt_transition_to_crash`, the `tnt_handle_*` accessors.

## Files

| File | What |
|---|---|
| `tnt.h` | public header (the whole API) |
| `tnt.c` | scheduler, typed arenas, generational handles, effect interpreter, I/O couriers |
| `echo.c` | the canonical TCP echo server (the example) |
| `test_tnt.c` | in-process dispatch test |
| `test_echo.sh` | end-to-end TCP echo round-trip harness |
| `Makefile` | links `../../build_unix/libxtc.a` |
