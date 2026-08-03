#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/micro/check.py -- compare a microbenchmark CSV run against the
# committed baseline, or (BENCH_UPDATE=1) write a fresh baseline.
#
# Invoked by check.sh; not meant to be run by hand.
#   argv[1] = baseline.json path
#   argv[2] = CSV run (name,ns_per_op,ops_per_sec ; leading '#'/header ignored)
# Env:
#   BENCH_UPDATE=1        write a new baseline from the CSV instead of checking
#   BENCH_TOLERANCE=x     override the allowed fractional slowdown (e.g. 0.25)

import json
import os
import sys

DEFAULT_TOLERANCE = 1.0  # +100%: the gate's job is to catch a hot path
#                          that ~DOUBLED (the stated goal), not micro-noise.
#                          Microbench variance on a shared/loaded host is
#                          large and one-sided; a tight tolerance false-
#                          positives every run.  A real 2x regression still
#                          trips this; ordinary noise does not.


def read_csv(path):
    """Parse the driver CSV into {name: ns_per_op}."""
    out = {}
    with open(path, encoding="ascii") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("name,"):
                continue
            parts = line.split(",")
            if len(parts) < 2:
                continue
            out[parts[0]] = float(parts[1])
    return out


def write_baseline(path, measured, tolerance):
    doc = {
        "_comment": (
            "Microbenchmark regression baseline -- MACHINE-RELATIVE. "
            "Regenerate on the target host with 'BENCH_UPDATE=1 "
            "bench/micro/check.sh'. See bench/micro/README.md. Values are "
            "median ns/op; 'tolerance' is the allowed fractional slowdown."
        ),
        "default_tolerance": tolerance,
        "benchmarks": {
            name: {"ns_per_op": round(ns, 2), "tolerance": tolerance}
            for name, ns in sorted(measured.items())
        },
    }
    with open(path, "w", encoding="ascii") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")
    print(f"check.py: wrote baseline for {len(measured)} benchmarks to {path}")


def main():
    baseline_path, csv_path = sys.argv[1], sys.argv[2]
    measured = read_csv(csv_path)
    if not measured:
        print("check.py: no benchmark rows in run output", file=sys.stderr)
        return 2

    env_tol = os.environ.get("BENCH_TOLERANCE") or ""
    override_tol = float(env_tol) if env_tol else None

    if os.environ.get("BENCH_UPDATE") == "1":
        write_baseline(baseline_path, measured, override_tol or DEFAULT_TOLERANCE)
        return 0

    try:
        with open(baseline_path, encoding="ascii") as fh:
            base = json.load(fh)
    except FileNotFoundError:
        print(
            f"check.py: baseline {baseline_path} missing; generate it with "
            "BENCH_UPDATE=1 bench/micro/check.sh",
            file=sys.stderr,
        )
        return 2

    file_default = base.get("default_tolerance", DEFAULT_TOLERANCE)
    bench = base.get("benchmarks", {})

    regressions = []
    missing = []
    print(f"{'benchmark':<18} {'baseline':>10} {'measured':>10} "
          f"{'delta':>8}  {'limit':>7}  status")
    for name in sorted(measured):
        got = measured[name]
        if name not in bench:
            missing.append(name)
            print(f"{name:<18} {'--':>10} {got:>10.2f} {'':>8}  {'':>7}  NEW")
            continue
        b = bench[name]
        want = float(b["ns_per_op"])
        tol = override_tol if override_tol is not None else float(
            b.get("tolerance", file_default))
        limit = want * (1.0 + tol)
        delta = (got - want) / want if want > 0 else 0.0
        bad = got > limit
        status = "REGRESSED" if bad else "ok"
        print(f"{name:<18} {want:>10.2f} {got:>10.2f} "
              f"{delta * 100:>+7.1f}% {tol * 100:>6.0f}%  {status}")
        if bad:
            regressions.append((name, want, got, delta, tol))

    # A benchmark in the baseline but absent from the run is also a problem
    # (it means the driver dropped or renamed a guarded path).
    dropped = sorted(set(bench) - set(measured))
    for name in dropped:
        print(f"{name:<18} {float(bench[name]['ns_per_op']):>10.2f} "
              f"{'MISSING':>10} {'':>8}  {'':>7}  DROPPED")

    if regressions:
        print()
        print(f"REGRESSION: {len(regressions)} benchmark(s) exceeded "
              "tolerance:", file=sys.stderr)
        for name, want, got, delta, tol in regressions:
            print(f"  {name}: {want:.2f} -> {got:.2f} ns/op "
                  f"({delta * 100:+.1f}%, limit +{tol * 100:.0f}%)",
                  file=sys.stderr)
        return 1
    if dropped:
        print()
        print(f"ERROR: {len(dropped)} baselined benchmark(s) missing from the "
              "run: " + ", ".join(dropped), file=sys.stderr)
        return 1

    print()
    print(f"OK: all {len(measured)} benchmarks within tolerance"
          + (f" ({len(missing)} new, not yet baselined)" if missing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
