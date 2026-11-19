#!/usr/bin/env python3
"""Aggregate coverage straight from gcov's JSON, merging N build dirs.

gcovr 8.2 silently drops some translation units (it reported 0 lines for
src/os/os_time.c etc. while `gcov` itself reports 36 lines / 75%), so the
authoritative numbers come from gcov's own --json-format output.
Line/branch/function are unioned per source file across every build dir,
which is what makes a cross-configuration (check + DST) figure meaningful.

gcov runs on every .gcno, not only on the .gcda files, so a translation
unit no test ran still counts its lines (as uncovered).

With --surface (the record scripts/dst_surface.py writes for the sim
build), it also prints the coverage of the DST-reachable surface, by every
tier and by the DST tier alone, with the rule that defines the surface.
"""

import argparse
import glob
import gzip
import json
import os
import subprocess

PREFIX = "/src/"


def gcov_records(build_dir):
    """Yield (tu, rel file, gcov file entry) for every src/ file of every
    translation unit gcov reports in build_dir."""
    gcnos = sorted(glob.glob(os.path.join(build_dir, "*.gcno")))
    if not gcnos:
        return
    subprocess.run(["gcov", "--json-format", "-b", "-c", "--branch-counts",
                    "--hash-filenames", "--object-directory", build_dir] + gcnos,
                   cwd=build_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for path in sorted(glob.glob(os.path.join(build_dir, "*.gcov.json.gz"))):
        try:
            with gzip.open(path) as f:
                data = json.load(f)
        except (OSError, ValueError) as e:
            print("coverage_summary: skipping unreadable %s: %s" % (path, e))
            continue
        tu = os.path.basename(data.get("data_file", path)).split(".")[0]
        for entry in data.get("files", []):
            if PREFIX in entry["file"]:
                yield tu, entry["file"].split(PREFIX, 1)[1].lstrip("/"), entry


def empty_counts():
    """Per-file counters: line, (line, branch index), and function name."""
    return {"lines": {}, "branches": {}, "funcs": {}}


def add_entry(counts, entry, keep=None):
    """Union one gcov file entry into counts (a hit anywhere is a hit).

    keep, when given, is (line_ok, func_ok): predicates on a gcov line
    record and a function record that select the surface's part.
    """
    for ln in entry.get("lines", []):
        if keep is not None and not keep[0](ln):
            continue
        no = ln["line_number"]
        counts["lines"][no] = counts["lines"].get(no, 0) or ln.get("count", 0)
        for bi, br in enumerate(ln.get("branches") or []):
            key = (no, bi)
            counts["branches"][key] = counts["branches"].get(key, 0) or br.get("count", 0)
    for fn in entry.get("functions", []):
        if keep is not None and not keep[1](fn):
            continue
        name = fn.get("demangled_name") or fn.get("name")
        counts["funcs"][name] = counts["funcs"].get(name, 0) or fn.get("execution_count", 0)


def surface_filter(surface, tu, rel):
    """The (line_ok, func_ok) predicates for one translation unit's file, or
    None when the unit has no function on the surface."""
    ent = surface["tus"].get(tu)
    if ent is None:
        return None
    reached = set(ent["functions"])
    cut = {}
    for fn, locs in ent["cut_lines"].items():
        cut[fn] = {line for path, line in locs if path == rel}

    def line_ok(ln):
        fn = ln.get("function_name")
        return fn in reached and ln["line_number"] not in cut.get(fn, ())

    def func_ok(fn):
        return fn.get("name") in reached
    return line_ok, func_ok


def totals(files):
    """Sum per-file counters: (lines, hit, branches, hit, funcs, hit)."""
    t = [0] * 6
    for c in files.values():
        for i, key in enumerate(("lines", "branches", "funcs")):
            t[2 * i] += len(c[key])
            t[2 * i + 1] += sum(1 for v in c[key].values() if v)
    return t


def pct(hit, tot):
    """A percentage, 0 for an empty set."""
    return 100.0 * hit / tot if tot else 0.0


def print_triplet(indent, t):
    """Print line, branch and function percentages from totals()."""
    for i, label in enumerate(("line", "branch", "function")):
        print("%s%-8s %6.1f%%   (%d/%d)" % (indent, label, pct(t[2 * i + 1], t[2 * i]),
                                            t[2 * i + 1], t[2 * i]))


def surface_keys(surface, sim_records):
    """The surface's (file, line), (file, line, branch) and (file, function)
    keys, decided from the sim build's own gcov records."""
    keys = {}
    for tu, rel, entry in sim_records:
        flt = surface_filter(surface, tu, rel)
        if flt is not None:
            add_entry(keys.setdefault(rel, empty_counts()), entry, flt)
    return keys


def restrict(files, keys):
    """files' counters at exactly the surface's keys (0 where absent)."""
    out = {}
    for rel, k in keys.items():
        c = files.get(rel, empty_counts())
        out[rel] = {kind: {key: c[kind].get(key, 0) for key in k[kind]}
                    for kind in ("lines", "branches", "funcs")}
    return out


def print_surface(surface_path, records, merged):
    """Print the DST-reachable surface figure (every tier, DST tier alone)."""
    with open(surface_path) as f:
        surface = json.load(f)
    sim = [r for bd, recs in records.items() if os.path.realpath(bd) == surface["build"]
           for r in recs]
    if not sim:
        print("\nDST-reachable surface: NOT MEASURED (the sim build %s is not among"
              " the build dirs)" % surface["build"])
        return
    keys = surface_keys(surface, sim)
    sim_files = {}
    for _, rel, entry in sim:
        add_entry(sim_files.setdefault(rel, empty_counts()), entry)
    print("\nDST-reachable public surface (%d source file(s)):" % len(keys))
    print("  covered by any tier:")
    print_triplet("    ", totals(restrict(merged, keys)))
    print("  covered by the DST tier alone:")
    print_triplet("    ", totals(restrict(sim_files, keys)))
    print("  rule:")
    for ln in surface["rule"]:
        print("    " + ln)


def print_failures(path):
    """Print the failing tests listed in path, one per line.

    A figure measured over a run in which tests failed undercounts what the
    passing suite covers, and must not be quoted as if the run were green.
    """
    with open(path) as f:
        fails = [ln.rstrip("\n") for ln in f if ln.strip()]
    if not fails:
        print("\ntests: all passed")
        return
    print("\ntests: %d FAILED -- this figure is from a run with failing tests:" % len(fails))
    for ln in fails:
        print("  " + ln)


def main():
    """Command line: merged totals, the worst files, the surface, failures."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--failures", metavar="FILE",
                    help="list of the run's failing tests, one per line")
    ap.add_argument("--surface", metavar="FILE",
                    help="the DST-reachable surface (scripts/dst_surface.py --out)")
    ap.add_argument("build_dirs", nargs="+")
    args = ap.parse_args()
    records = {bd: list(gcov_records(os.path.abspath(bd))) for bd in args.build_dirs}
    merged = {}
    for recs in records.values():
        for _, rel, entry in recs:
            add_entry(merged.setdefault(rel, empty_counts()), entry)
    print("MERGED coverage over %d source file(s), %d build dir(s):"
          % (len(merged), len(args.build_dirs)))
    print_triplet("  ", totals(merged))
    rows = []
    for rel, c in merged.items():
        t = totals({rel: c})
        rows.append((t[0] - t[1], t[2] - t[3], pct(t[1], t[0]), t[0], rel))
    print("\nworst by uncovered lines:")
    rows.sort(reverse=True)
    for miss_l, miss_b, p, n_lines, rel in rows[:15]:
        print("  %-28s %5.1f%%  miss L=%-5d B=%-5d (%d lines)"
              % (rel, p, miss_l, miss_b, n_lines))
    if args.surface:
        print_surface(args.surface, records, merged)
    if args.failures:
        print_failures(args.failures)


if __name__ == "__main__":
    main()
