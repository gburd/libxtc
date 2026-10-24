#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# scripts/perf-gate-judge.py -- the verdict for scripts/perf-gate.sh.
#
#   perf-gate-judge.py RESULTS.CSV DEFAULT_THRESH "name|hib|thresh ..."
#
# RESULTS.CSV rows: cell,rep,side,value,qual (side = base | cand).
# A cell REGRESSES when the candidate's median is worse than the base's by
# more than the cell's threshold AND the ranges do not overlap (every
# candidate rep worse than every base rep).  A cell missing on one side
# (a bench the base release predates) is reported, not judged.
# Exit 0 = no regression, 1 = at least one regression, 2 = no data.

import csv
import os
import statistics
import sys


def main():
    rows = list(csv.DictReader(open(sys.argv[1], encoding="ascii")))
    default = float(sys.argv[2])
    spec = {}
    for item in sys.argv[3].split():
        name, hib, th = item.split("|")
        spec[name] = (hib == "1", float(th) if th else default)
    data = {}
    qual = "qualified"
    for r in rows:
        v = float(r["value"])
        # A zero or negative reading is not a measurement: the bench did
        # not run that path on this side (e.g. a WAL mode the base
        # release does not have).  Leave the side out; the cell is then
        # reported as one-sided, not judged.
        if v <= 0:
            continue
        data.setdefault(r["cell"], {}).setdefault(r["side"], []).append(v)
        if r["qual"] != "qualified":
            qual = r["qual"]
    if not data:
        print("perf-gate: no results")
        return 2
    print("perf-gate verdict (%s host): median [min-max] per side; "
          "delta = candidate vs base, + is better" % qual)
    print("%-22s %-28s %-28s %8s  %s" % ("cell", "base", "candidate",
                                         "delta", "verdict"))
    # PERF_GATE_ACCEPT="cell ...": regressions the maintainer has decided
    # to ship.  They are still printed, as ACCEPTED, so the release notes
    # can name each one with its cause; they do not fail the gate.
    accept = set(os.environ.get("PERF_GATE_ACCEPT", "").split())
    regress = 0
    accepted = []
    for name in spec:
        if name not in data:
            continue
        hib, th = spec[name]
        b = data[name].get("base", [])
        c = data[name].get("cand", [])
        if not b or not c:
            print("%-22s %s" % (name, "only one side measured this "
                                "cell: not judged"))
            continue
        mb, mc = statistics.median(b), statistics.median(c)
        # delta > 0 means the candidate is better, whatever the unit
        delta = (mc - mb) / mb if hib else (mb - mc) / mb
        if hib:
            apart = max(c) < min(b)
        else:
            apart = min(c) > max(b)
        if delta < -th and apart and name in accept:
            verdict = "ACCEPTED regression"
            accepted.append(name)
        elif delta < -th and apart:
            verdict = "REGRESSION"
            regress += 1
        elif delta > th and (min(c) > max(b) if hib else max(c) < min(b)):
            verdict = "improved"
        else:
            verdict = "ok"
        print("%-22s %9.4g [%9.4g-%9.4g] %9.4g [%9.4g-%9.4g] %+7.1f%%  %s"
              % (name, mb, min(b), max(b), mc, min(c), max(c),
                 100 * delta, verdict))
    if accepted:
        print("perf-gate: accepted (name each in the release notes): %s"
              % " ".join(accepted))
    print("perf-gate: %d regression(s)" % regress)
    return 1 if regress else 0


if __name__ == "__main__":
    sys.exit(main())
