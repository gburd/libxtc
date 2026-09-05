#!/usr/bin/env python3
"""Aggregate coverage straight from gcov's JSON, merging N build dirs.

gcovr 8.2 silently drops some translation units (it reported 0 lines for
src/os/os_time.c etc. while `gcov` itself reports 36 lines / 75%), so the
authoritative numbers come from gcov's own --json-format output.
Line/branch/function are unioned per source file across every build dir,
which is what makes a cross-configuration (check + DST) figure meaningful.
"""
import glob, gzip, json, os, subprocess, sys, collections

def collect(build_dir, want_prefix):
    files = {}
    gcdas = glob.glob(os.path.join(build_dir, "*.gcda"))
    if not gcdas:
        return files
    subprocess.run(["gcov", "--json-format", "-b", "-c", "--branch-counts",
                    "--hash-filenames", "--object-directory", build_dir]
                   + gcdas, cwd=build_dir,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for j in glob.glob(os.path.join(build_dir, "*.gcov.json.gz")):
        try:
            d = json.load(gzip.open(j))
        except Exception:
            continue
        for f in d.get("files", []):
            path = f["file"]
            if want_prefix not in path:
                continue
            rel = path.split(want_prefix, 1)[1].lstrip("/")
            ent = files.setdefault(rel, {"lines": {}, "branches": {},
                                         "funcs": {}})
            for ln in f.get("lines", []):
                no = ln["line_number"]
                ent["lines"][no] = ent["lines"].get(no, 0) or ln.get("count", 0)
                for bi, br in enumerate(ln.get("branches", []) or []):
                    key = (no, bi)
                    ent["branches"][key] = (ent["branches"].get(key, 0)
                                            or br.get("count", 0))
            for fn in f.get("functions", []):
                nm = fn.get("demangled_name") or fn.get("name")
                ent["funcs"][nm] = (ent["funcs"].get(nm, 0)
                                    or fn.get("execution_count", 0))
    return files

def merge(a, b):
    for rel, e in b.items():
        t = a.setdefault(rel, {"lines": {}, "branches": {}, "funcs": {}})
        for k, v in e["lines"].items():
            t["lines"][k] = t["lines"].get(k, 0) or v
        for k, v in e["branches"].items():
            t["branches"][k] = t["branches"].get(k, 0) or v
        for k, v in e["funcs"].items():
            t["funcs"][k] = t["funcs"].get(k, 0) or v
    return a

def pct(cov, tot):
    return 100.0 * cov / tot if tot else 0.0

if __name__ == "__main__":
    prefix = "/src/"
    total = {}
    for bd in sys.argv[1:]:
        total = merge(total, collect(bd, prefix))
    L = Lc = B = Bc = F = Fc = 0
    rows = []
    for rel, e in sorted(total.items()):
        l = len(e["lines"]); lc = sum(1 for v in e["lines"].values() if v)
        b = len(e["branches"]); bc = sum(1 for v in e["branches"].values() if v)
        fn = len(e["funcs"]); fc = sum(1 for v in e["funcs"].values() if v)
        L += l; Lc += lc; B += b; Bc += bc; F += fn; Fc += fc
        rows.append((l - lc, b - bc, pct(lc, l), l, rel))
    print("MERGED coverage over %d source file(s), %d build dir(s):"
          % (len(total), len(sys.argv) - 1))
    print("  line     %6.1f%%   (%d/%d)" % (pct(Lc, L), Lc, L))
    print("  branch   %6.1f%%   (%d/%d)" % (pct(Bc, B), Bc, B))
    print("  function %6.1f%%   (%d/%d)" % (pct(Fc, F), Fc, F))
    if "-v" in os.environ.get("COVSUM_OPTS", ""):
        pass
    print("\nworst by uncovered lines:")
    rows.sort(reverse=True)
    for lm, bm, p, l, rel in rows[:15]:
        print("  %-28s %5.1f%%  miss L=%-5d B=%-5d (%d lines)"
              % (rel, p, lm, bm, l))
