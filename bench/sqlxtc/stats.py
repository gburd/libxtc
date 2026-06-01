#!/usr/bin/env python3
# bench/sqlxtc/stats.py -- aggregate mvcc_bench JSONL into a report.
#
# Copyright (c) 2026, The XTC Project -- ISC License.
#
# Reads one or more results/*.jsonl files, groups runs by
# (host, label, read_pct, cores), and reports the median and the
# interquartile range across repetitions for throughput and the
# latency percentiles -- median-of-reps is robust to the occasional
# scheduler outlier, and the IQR shows run-to-run variability (the
# point of the exercise being LOW p99 variance).
#
# Usage:  python3 stats.py results/*.jsonl

import json
import statistics
import sys
from collections import defaultdict


def med(xs):
    return statistics.median(xs) if xs else 0.0


def iqr(xs):
    if len(xs) < 2:
        return 0.0
    xs = sorted(xs)
    n = len(xs)
    q1 = xs[n // 4]
    q3 = xs[(3 * n) // 4]
    return q3 - q1


def main(paths):
    rows = []
    for p in paths:
        with open(p) as f:
            for line in f:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
    if not rows:
        print("no data")
        return

    groups = defaultdict(list)
    for r in rows:
        key = (r.get("host", "?"), r.get("read_pct"), r.get("cores"))
        groups[key].append(r)

    print("# sqlxtc MVCC engine -- aggregated results "
          "(median across reps, IQR in brackets)\n")
    hosts = sorted({k[0] for k in groups})
    for host in hosts:
        print(f"## host: {host}\n")
        print("| read% | cores | kops/s (IQR) | p50 us | p99 us | p99.9 us | "
              "max us | reps |")
        print("|------:|------:|-------------:|-------:|-------:|---------:|"
              "-------:|-----:|")
        keys = sorted([k for k in groups if k[0] == host],
                      key=lambda k: (-(k[1] or 0), k[2] or 0))
        for k in keys:
            g = groups[k]
            kops = [x["kops_per_sec"] for x in g]
            print("| %d | %d | %.1f [%.1f] | %.1f | %.1f | %.1f | %.1f | %d |"
                  % (k[1], k[2], med(kops), iqr(kops),
                     med([x["p50_us"] for x in g]),
                     med([x["p99_us"] for x in g]),
                     med([x["p999_us"] for x in g]),
                     med([x["max_us"] for x in g]),
                     len(g)))
        print()
        # Scaling note: throughput vs cores at the top read mix.
        top = max((k[1] for k in keys), default=0)
        line = [(k[2], med([x["kops_per_sec"] for x in groups[k]]))
                for k in keys if k[1] == top]
        line.sort()
        if len(line) > 1:
            base = line[0][1] or 1.0
            scal = ", ".join("%dc=%.2fx" % (c, v / base) for c, v in line)
            print(f"scaling @ read%={top} (vs {line[0][0]} core): {scal}\n")


if __name__ == "__main__":
    main(sys.argv[1:] or ["results"])
