#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project — All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/conformance/plot.py
#   Read results.csv from stdin; produce visualisations for the 7
#   conformance workloads.
#
# Usage:
#   ./bench/conformance/run.sh | python3 bench/conformance/plot.py
#   python3 bench/conformance/plot.py < results.csv
#
# Output (in priority order):
#   1. results.html  — if matplotlib + mpld3 are available (interactive).
#   2. bench/conformance/plots/*.png  — one PNG per workload if matplotlib
#      is available but mpld3 is not.
#   3. Tabular text summary to stdout — if matplotlib is not installed.
#
# Python 3.8+ standard library only; matplotlib/mpld3 are optional.
#
# Notes:
#   - Workloads with no data rows are silently skipped.
#   - The script always exits 0; it prints a warning and falls back
#     gracefully when optional dependencies are missing.

import csv
import os
import sys
from collections import defaultdict

# ---------------------------------------------------------------------------
# Optional dependency probes
# ---------------------------------------------------------------------------
try:
    import matplotlib
    matplotlib.use("Agg")          # non-interactive backend, safe everywhere
    import matplotlib.pyplot as plt
    import matplotlib.gridspec as gridspec
    HAS_MPL = True
except ImportError:
    HAS_MPL = False

try:
    import mpld3
    HAS_MPLD3 = True
except ImportError:
    HAS_MPLD3 = False

# ---------------------------------------------------------------------------
# CSV schema
# ---------------------------------------------------------------------------
COLUMNS = [
    "workload", "runtime", "params",
    "elapsed_ns", "cpu_us", "rss_kb",
    "p50_ns", "p95_ns", "p99_ns", "p999_ns",
]

INT_COLS = {"elapsed_ns", "cpu_us", "rss_kb", "p50_ns", "p95_ns", "p99_ns", "p999_ns"}

WORKLOAD_ORDER = ["W1", "W2", "W3", "W4", "W5", "W6", "W7"]
WORKLOAD_NAMES = {
    "W1": "W1 spawn-N-await-all",
    "W2": "W2 echo server",
    "W3": "W3 mailbox ping-pong",
    "W4": "W4 mutex contention",
    "W5": "W5 reader/writer ratio",
    "W6": "W6 tail latency",
    "W7": "W7 timer wheel",
}
RUNTIMES    = ["xtc", "tokio", "erlang"]
RT_COLOURS  = {"xtc": "#1f77b4", "tokio": "#ff7f0e", "erlang": "#2ca02c"}

# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_csv(fh):
    """Return list-of-dicts from the CSV; skip comment lines and blanks."""
    rows = []
    reader = csv.DictReader(
        (line for line in fh if not line.startswith("#") and line.strip()),
    )
    for row in reader:
        # Coerce integer columns, default missing to 0.
        for col in INT_COLS:
            try:
                row[col] = int(row.get(col, 0) or 0)
            except ValueError:
                row[col] = 0
        rows.append(row)
    return rows


def group_by_workload(rows):
    """Return {workload_prefix: [row, ...]} where prefix is e.g. 'W1'."""
    groups = defaultdict(list)
    for row in rows:
        wl = row.get("workload", "").upper()
        # Accept 'W1', 'w1', 'W1_spawn', etc.
        prefix = wl.split("_")[0].upper()
        if not prefix.startswith("W"):
            continue
        groups[prefix].append(row)
    return groups


# ---------------------------------------------------------------------------
# Matplotlib plots
# ---------------------------------------------------------------------------

def _ns_label(ns_val):
    """Human-readable nanosecond string."""
    if ns_val >= 1_000_000_000:
        return f"{ns_val/1e9:.2f} s"
    if ns_val >= 1_000_000:
        return f"{ns_val/1e6:.2f} ms"
    if ns_val >= 1_000:
        return f"{ns_val/1e3:.2f} µs"
    return f"{ns_val} ns"


def plot_workload(ax_bar, ax_line, wl_key, rows):
    """
    Draw a bar chart of elapsed_ns (left axis) and a line chart of
    p50/p99/p999 latency (right axis) for one workload.
    """
    runtimes = [r for r in RUNTIMES if any(row["runtime"] == r for row in rows)]
    if not runtimes:
        return

    elapsed  = [next((row["elapsed_ns"]  for row in rows if row["runtime"] == rt), 0) for rt in runtimes]
    p50s     = [next((row["p50_ns"]      for row in rows if row["runtime"] == rt), 0) for rt in runtimes]
    p99s     = [next((row["p99_ns"]      for row in rows if row["runtime"] == rt), 0) for rt in runtimes]
    p999s    = [next((row["p999_ns"]     for row in rows if row["runtime"] == rt), 0) for rt in runtimes]
    colours  = [RT_COLOURS.get(rt, "#999999") for rt in runtimes]
    x        = list(range(len(runtimes)))

    # Bar: elapsed_ns
    ax_bar.bar(x, elapsed, color=colours, alpha=0.8, zorder=2)
    ax_bar.set_xticks(x)
    ax_bar.set_xticklabels(runtimes, fontsize=8)
    ax_bar.set_ylabel("elapsed_ns", fontsize=8)
    ax_bar.set_title(WORKLOAD_NAMES.get(wl_key, wl_key), fontsize=9)
    ax_bar.yaxis.grid(True, linestyle="--", alpha=0.4, zorder=1)
    ax_bar.set_axisbelow(True)

    # Line: percentiles
    if any(v > 0 for v in p50s + p99s + p999s):
        ax_line.plot(x, p50s,  "o--", color="#d62728", label="p50",  linewidth=1.2)
        ax_line.plot(x, p99s,  "s--", color="#9467bd", label="p99",  linewidth=1.2)
        ax_line.plot(x, p999s, "^--", color="#8c564b", label="p999", linewidth=1.2)
        ax_line.set_ylabel("latency (ns)", fontsize=8)
        ax_line.set_xticks(x)
        ax_line.set_xticklabels(runtimes, fontsize=8)
        ax_line.legend(fontsize=7, loc="upper right")
        ax_line.yaxis.grid(True, linestyle=":", alpha=0.4)


def make_plots(groups, out_dir=None):
    """
    Build all plots.  Returns HTML string if mpld3 is available,
    or writes PNG files and returns None.
    """
    active = [wl for wl in WORKLOAD_ORDER if groups.get(wl)]
    if not active:
        print("plot.py: no data — nothing to plot.", file=sys.stderr)
        return None

    n = len(active)
    fig = plt.figure(figsize=(5 * n, 8))
    gs  = gridspec.GridSpec(2, n, figure=fig, hspace=0.55, wspace=0.45)

    for col, wl_key in enumerate(active):
        ax_bar  = fig.add_subplot(gs[0, col])
        ax_line = fig.add_subplot(gs[1, col])
        plot_workload(ax_bar, ax_line, wl_key, groups[wl_key])

    fig.suptitle("XTC Conformance Benchmarks — xtc vs Tokio vs Erlang", fontsize=11)

    if HAS_MPLD3:
        html = mpld3.fig_to_html(fig)
        plt.close(fig)
        return html
    else:
        # Fall back to per-workload PNG files.
        if out_dir is None:
            out_dir = os.path.join(
                os.path.dirname(os.path.abspath(__file__)), "plots"
            )
        os.makedirs(out_dir, exist_ok=True)

        for col, wl_key in enumerate(active):
            fig_single, (ax_b, ax_l) = plt.subplots(2, 1, figsize=(5, 6))
            fig_single.suptitle(WORKLOAD_NAMES.get(wl_key, wl_key), fontsize=10)
            plot_workload(ax_b, ax_l, wl_key, groups[wl_key])
            out_path = os.path.join(out_dir, f"{wl_key.lower()}.png")
            fig_single.savefig(out_path, dpi=120, bbox_inches="tight")
            plt.close(fig_single)
            print(f"plot.py: wrote {out_path}", file=sys.stderr)

        plt.close(fig)
        return None


# ---------------------------------------------------------------------------
# Plain-text fallback
# ---------------------------------------------------------------------------

def print_table(groups):
    """Print a readable table when matplotlib is not available."""
    METRIC_COLS = ["elapsed_ns", "cpu_us", "rss_kb", "p50_ns", "p99_ns", "p999_ns"]
    col_w = 14

    print(f"\n{'XTC Conformance Benchmark Results':^{7 + len(METRIC_COLS)*col_w}}")
    print()

    active = [wl for wl in WORKLOAD_ORDER if groups.get(wl)]
    if not active:
        print("  (no data)")
        return

    header = f"  {'workload':<10}{'runtime':<10}" + "".join(f"{c:>{col_w}}" for c in METRIC_COLS)
    print(header)
    print("  " + "-" * (len(header) - 2))

    for wl_key in active:
        for row in sorted(groups[wl_key], key=lambda r: r.get("runtime", "")):
            rt  = row.get("runtime", "?")
            wl  = row.get("workload", wl_key)
            line = f"  {wl:<10}{rt:<10}"
            for col in METRIC_COLS:
                val = row.get(col, 0)
                if isinstance(val, int) and val >= 1000:
                    line += f"{val:>{col_w},}"
                else:
                    line += f"{val!s:>{col_w}}"
            print(line)
        print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    rows   = load_csv(sys.stdin)
    groups = group_by_workload(rows)

    if not HAS_MPL:
        print(
            "plot.py: matplotlib not found — printing tabular summary.",
            file=sys.stderr,
        )
        print_table(groups)
        return

    html = make_plots(groups)
    if html is not None:
        out_path = "results.html"
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(html)
        print(f"plot.py: wrote {out_path}", file=sys.stderr)
    elif not any(groups.get(wl) for wl in WORKLOAD_ORDER):
        print("plot.py: CSV contained no recognised workload rows.", file=sys.stderr)


if __name__ == "__main__":
    main()
