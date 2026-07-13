# tools/sim-monitor -- a VOPR-inspired DST trace viewer for libxtc

A small, playful, **strictly optional** visualizer for libxtc's
deterministic-simulation traces, in the spirit of TigerBeetle's VOPR
visualizer: render a DST run instead of reading a log. Not required to
build the library, not wired into `make check`, and not a correctness
mechanism -- the simulator and its assertions are that; this is a
debugging/demo aid layered on top.

Full design rationale, the "which 80s game" discussion, and the phased
plan are in `PLAN.md` section 19.25 and `.agent/DST_MATURITY_2026-07.md`.

## What it looks like

One horizontal lane per simulated `xtc_loop`. A lane lights up green
briefly on a `run`/`wake`/message event, blue for a send/recv, and
flashes **red** across the whole lane when a named buggify site
activates on it -- closer to Missile Command's "an attack lands on a
city" than a literal physics sim, matching the message-passing,
lane-based shape of the actual runtime (and of VOPR itself).

## Two pieces

- **`recorder.c`** -- links against a `--with-io-backend=sim` build of
  libxtc, runs a small multi-loop ping/pong scenario with buggify
  enabled, and dumps an `xtc_tail` trace (SCHED + SIM sources) to a
  file. This is a stand-in for "point it at a real DST test's trace";
  any program that calls `xtc_tail_enable`/`xtc_tail_dump` produces a
  file this viewer can read.
- **`viewer.c`** -- a standalone raylib program (no libxtc dependency)
  that parses the `xtc_tail` v2 binary format directly (documented in
  `src/inc/xtc_tail.h`) and replays it. Controls: SPACE pause/resume,
  LEFT/RIGHT step one event, UP/DOWN change replay speed, ESC quit.

Replay, not live attach: the whole point of DST is that a seed replays
byte-identically, so "watch seed 12345" is a recorded file you can
re-run and step through, not a process you attach to mid-flight.

## Building

Needs `pkg-config` and `raylib` (5.x). Two ways to get raylib:

```sh
# Nix (this repo ships a dedicated shell so the main dev shell stays
# free of a GUI-toolkit dependency most contributors do not need):
nix develop .#sim-monitor

# or on any system with raylib installed via your package manager
# (apt/brew/pacman all have a raylib package):
cd tools/sim-monitor && make
```

`make` builds both `recorder` and `viewer` into this directory
(gitignored; see `tools/sim-monitor/.gitignore`).

## Running it

```sh
# 1. Build a --with-io-backend=sim libxtc.a somewhere (any seed's fine
#    for a demo trace; a real DST test would call xtc_tail_enable +
#    xtc_tail_dump itself instead of using the bundled recorder):
mkdir -p /tmp/xtcsim && cd /tmp/xtcsim
/path/to/libxtc/dist/configure --with-io-backend=sim --with-tls=none --without-liburing
make -j

# 2. Record a trace with the bundled scenario:
cd /path/to/libxtc/tools/sim-monitor
gcc -std=c11 -I../../src/inc -o recorder recorder.c /tmp/xtcsim/libxtc.a -lm -pthread
./recorder 9 /tmp/trace.bin      # seed 9 produces a lively one (buggify fires 1x)

# 3. Watch it:
make viewer   # or the gcc line in Makefile if not using make
./viewer /tmp/trace.bin
```

## Phase 1 vs Phase 2 (current status: Phase 1)

- **Phase 1 (this): a live/recorded "watch it run" animation.**  Lanes,
  activity flashes, buggify flashes.  No scrubbing.
- **Phase 2 (not started): scrubbable replay.**  Step a recorded seed's
  run backward AND forward through actual reconstructed state (not
  just "jump to event N in a forward-only stream", which Phase 1
  already does via LEFT/RIGHT) -- this needs either periodic state
  snapshots or a recompute-from-seed-to-tick-N approach, and is
  genuinely the harder half of what VOPR itself had to build.  See
  PLAN.md 19.25 for the full phase breakdown.

## Honest limitation of this v1

The "activity" indicator per lane is a net spawn-count pulse (grows on
spawn, decays over time, shrinks on exit) -- a cheap "how busy is this
lane" signal, not a literal per-proc dot with a tracked position. A
faithful per-proc view (a stable dot per live `xtc_pid_t`, moving
between drawn zones for run-queue/parked/blocked-on-recv) is future
work once the trace format carries enough state-transition detail to
place a dot precisely; XTC_TAIL_PARK exists in the format already for
exactly this but is not yet consumed by the viewer.

## Verifying this without a display

The Nix sandbox this was built in cannot open a real GL/EGL context
(headless, no GPU), so the window-drawing code could not be visually
confirmed at development time -- an honest limitation of THIS
environment, not evidence the tool doesn't work (a normal desktop or a
GitHub Actions runner with a real or virtual GPU has a working
GL/EGL/GLX stack). What WAS verified here: the trace-parsing logic
(shared verbatim with `viewer.c`) round-trips 10 recorded traces
correctly -- monotonic timestamps reconstructed from the delta
encoding, and buggify-activation counts matching the recorder's own
reported counts exactly. If you build this on a real machine and the
window does not open, that is a real bug to report; if it opens and
the lanes don't look right, that's very possibly real too -- this has
not had a human look at the rendered output yet.
