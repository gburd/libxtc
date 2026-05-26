# OTP test port

Selected test cases ported from Erlang/OTP's standard test suites
(`lib/stdlib/test/`, `lib/kernel/test/`) into C using the xtc API.
Demonstrates that xtc's process/supervisor/gen_server contracts
match OTP's behaviour for the foundational cases.

## What's ported

| File | OTP source | Cases | Status |
|---|---|---|---|
| `test_otp_proc_lib.c` | `proc_lib_SUITE.erl` (1069 lines) | 8 | all pass |
| `test_otp_gen_server.c` | `gen_server_SUITE.erl` (3572 lines) | 4 | all pass |
| `test_otp_supervisor.c` | `supervisor_SUITE.erl` (4052 lines) | 7 | all pass |

## What's omitted (BEAM-only or out-of-scope)

OTP tests rely heavily on BEAM features that don't exist in C:

| Feature | Reason omitted |
|---|---|
| Hot code reload (`code_change`) | xtc has no module loader |
| Process hibernation | no BEAM-style heap to suspend |
| Distributed nodes (`call_remote*`) | xtc is single-node |
| Global registration (`global:`) | use `xtc_reg` for local |
| `format_status` callbacks | no introspection contract |
| `format_log_*` (Logger integration) | xtc has no logger |
| Erlang's `proc_lib:initial_call`/process-dict | no per-proc dictionary |
| Tree of nested supervisors (`tree`) | mechanically straightforward; not yet ported |
| Hanging-restart-loop tests | depend on Erlang's specific timer behaviour |

## OTP coverage breakdown

What the foundational tests verify against the Erlang reference:

### `proc_lib`

* **`t_spawn`** — `spawn/3` returns a valid Pid; the spawned proc runs and exits normally.
* **`spawn_link_abnormal`** — abnormal exit from a linked process delivers an EXIT signal to the linker (xtc's `xtc_link` + recv on 'E' envelopes).
* **`spawn_monitor_down`** — monitor delivers a DOWN message on target death.
* **`self_returns_pid`** — `self()` from inside a spawned proc returns a non-bottom Pid.
* **`pingpong_50`** — 50 round-trip messages between two processes; encodes sender Pid in payload (xtc has no implicit reply-to).
* **`recv_timeout`** — `receive ... after Timeout` maps to `xtc_recv` returning `XTC_E_AGAIN`.
* **`selective_receive`** — out-of-order match: take 42 first, then drain 1, 2, 3, 4 in arrival order.  Save-queue semantics.
* **`fanout_100`** — spawn 100 workers, send each a marker, every one runs.

### `gen_server` (xtc_svr)

* **`start_stop`** — `gen_server:start_link` calls `init`; `gen_server:stop` calls `terminate`.
* **`call_cast_info`** — sync `call`, async `cast`, raw `info` messages all dispatch to the right callbacks.
* **`stop_via_handle_call`** — returning `XTC_SVR_STOP` from a callback ends the server cleanly.
* **`call_timeout`** — call returns `XTC_E_AGAIN` if the server doesn't reply in time.

### `supervisor`

* **`one_for_one_basic`** — A and B; A crashes twice, B unaffected; A restarted twice.
* **`one_for_all`** — A crashes; B is also killed and restarted.
* **`rest_for_one_strategy_accepts`** — strategy enum is honoured; cascade is tested in `test/m10/test_sup.c`.
* **`permanent_restarts_on_normal`** — PERMANENT child is restarted even on normal exit.
* **`temporary_no_restart`** — TEMPORARY child is never restarted (even after abnormal exit).
* **`intensity_exceeded`** — flapping child triggers supervisor's own exit.
* **`count_children`** — `xtc_sup_n_children` reports the right number.

## OTP semantics that map verbatim

Despite the syntactic differences, these xtc concepts are
behaviourally identical to OTP:

| OTP | xtc |
|---|---|
| `Pid = spawn(Fun)` | `xtc_proc_spawn(loop, fn, arg, NULL, &pid)` |
| `Pid ! Msg` | `xtc_send(pid, msg, size)` |
| `receive ... end` | `xtc_recv(&m, &sz, timeout)` |
| `receive Pat -> ... end` | `xtc_recv_match(match_fn, &m, &sz, timeout)` |
| `link(Pid)` | `xtc_link(pid)` |
| `Ref = monitor(process, Pid)` | `xtc_monitor(pid, &ref)` |
| `exit(Pid, Reason)` | `xtc_exit_pid(pid, reason)` |
| `exit(Reason)` | `xtc_exit_self(reason)` |
| `self()` | `xtc_self()` |
| `gen_server:call(Pid, Req, Timeout)` | `xtc_svr_call(pid, req, sz, &reply, &rs, timeout)` |
| `gen_server:cast(Pid, Msg)` | `xtc_svr_cast(pid, msg, sz)` |
| `supervisor:start_link({{Strategy, MaxR, MaxT}, Children})` | `xtc_sup_start(loop, &opts, kids, n, &sup)` |
| `application:start(App)` | `xtc_app_create + xtc_app_start + xtc_app_run` |

## Future work

To complete OTP parity, port the remaining cases:

* `gen_server`: ~30 more cases (multicall, parallel, format_status,
  send_request_*).  Would push the gen_server module to ~95% match.
* `supervisor`: ~25 more cases (significant children, code_change,
  scale_start_stop_many_children, hanging_restart_loop variants).
* `gen_event` (not yet started): ~20 cases — but xtc has no
  gen_event analogue; need to design `xtc_event` first.
* `application_SUITE`: ~50 cases for app-lifecycle.  Xtc's `xtc_app`
  is simpler; many cases test BEAM lifecycle hooks we don't have.

The 19 cases here are the ones that demonstrate xtc's contract for
the M8/M10/M10.5 layer is consistent with OTP's well-known semantics.
