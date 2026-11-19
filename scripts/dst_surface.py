#!/usr/bin/env python3
"""Compute libxtc's DST-reachable surface from a sim build's gcc dumps.

AGENTS.md states the coverage claim as ">85% of the DST-reachable public
surface".  This program defines that surface mechanically, so the figure is
recomputed from the code on every run instead of from a hand-kept list that
rots.  The surface is the set of src/ lines (and their branches and
functions) that the deterministic simulator can execute:

  R0  Roots: the global functions of the sim build that dist/libxtc.map
      exports (xtc_*, plus the internals a public macro expands to).  That
      is the public API, as test/m0/test_symbols.sh [C5]-[C8] hold it.
  R1  Configuration: the sim build is configured as test/sim/run_sim_tests.sh
      configures it (--with-io-backend=sim --with-tls=none
      --without-liburing).  The native I/O backends and the real TLS
      backends compile to nothing or to NOSYS stubs there, so their code is
      not in the surface at all.
  R2  Edges: a reached function reaches every function it calls directly,
      every function whose address it takes (a callback), and every
      function that a variable it uses holds in its initializer (a static
      ops table), transitively.
  R3  Simulation gates: __xtc_sim_active() and xtc_sim_active() are true
      under the simulator, so a block that only the "not simulating"
      outcome of a test of one of them reaches is cut, with its lines and
      calls.  That is the thread-pool offload in xtc_blocking_run, the aio
      offload, the real fork in xtc_osproc_spawn, and the like.
  R4  Thread entries: a function passed to __os_thread_create,
      __os_pthread_create_masked, or pthread_create runs on an OS thread
      the simulator does not schedule.  The creating call contributes no
      edge, so the thread body (the blocking pool's workers, the exec
      workers, the deadlock detector, the PSI and signal threads) is cut
      unless something else reaches it.

Every heuristic here errs toward inclusion: an indirect call the dumps do
not show, a gate written in a shape R3 does not recognize (the result kept
in a variable and tested in a later block), or a name that happens to match
a function keeps code IN the surface, which can only lower the figure.  Code
that is in-process but needs a real kernel object still counts (the slab's
shared-memory mode, the xproc parent path past xtc_osproc_spawn's NOSYS
under the simulator): no rule cuts it, and it is listed as known residue in
AGENTS.md rather than excluded by hand.

The input is gcc's own view of the code: each library object's
-fdump-ipa-cgraph (symbols, visibility, and variable initializers) and
-fdump-tree-cfg-blocks-lineno (each function's basic blocks, their
successors, statements, and source lines).  scripts/coverage.sh builds the
sim tier with both, then runs this program, which writes a JSON file that
scripts/coverage_summary.py intersects with gcov's data.  --self-test
compiles a fixture and checks every rule, so a change in gcc's dump format
fails loudly instead of skewing the figure.
"""

import argparse
import fnmatch
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

GATES = ("__xtc_sim_active", "xtc_sim_active")
THREAD_CREATE = re.compile(
    r"(?<![\w.>])(__os_thread_create|__os_pthread_create_masked|pthread_create) \(")
NODE_RE = re.compile(r"^(\S+)/\d+ \(.*\)$")
REF_RE = re.compile(r"(\S+)/\d+ \((?:addr|read|write)\)")
FUNC_RE = re.compile(r"^;; Function (\S+) \(")
SUCC_RE = re.compile(r"^;; (\d+) succs \{([\d ]*)\}")
BB_RE = re.compile(r"^;;   basic block (\d+),")
LOC_RE = re.compile(r"\[([^\]\s]+):(\d+):\d+[^\]]*\]")
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
IDENT_RE = re.compile(r"(?<![\w.>])[A-Za-z_]\w*")
GATE_CALL_RE = re.compile(r"(\S+) = (?:%s) \(\);" % "|".join(GATES))
SRC_PREFIX = "/src/"


def rel_path(path):
    """Map a source path to its src/-relative form, as coverage_summary.py
    does, or None for a file outside src/ (a system header)."""
    if SRC_PREFIX not in path:
        return None
    return path.split(SRC_PREFIX, 1)[1].lstrip("/")


def parse_cgraph(path):
    """Read the first "Reclaimed Symbol table" of one -fdump-ipa-cgraph file.

    Returns {name: {"kind": "func"|"var", "def": bool, "public": bool,
    "refs": set of names}}.  Two statics of one name (a static local in two
    functions) share an entry, which can only add references.
    """
    syms = {}
    cur = None
    in_table = False
    with open(path, errors="replace") as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if not in_table:
                in_table = ln.startswith("Reclaimed Symbol table:")
                continue
            m = NODE_RE.match(ln)
            if m:
                cur = syms.setdefault(m.group(1), {"kind": "", "def": False,
                                                   "public": False,
                                                   "refs": set()})
                continue
            if ln and not ln.startswith(" "):
                if syms:
                    break
                continue
            if cur is None:
                continue
            s = ln.strip()
            if s.startswith("Type:"):
                cur["kind"] = "var" if "variable" in s else "func"
                cur["def"] = cur["def"] or "definition" in s
            elif s.startswith("Visibility:"):
                words = s.split()
                cur["public"] = cur["public"] or (
                    "public" in words and "external" not in words)
            elif s.startswith("References:"):
                cur["refs"].update(REF_RE.findall(s))
    return syms


def new_func():
    """An empty parsed function: blocks, successor lists, entry block."""
    return {"blocks": {}, "succs": {}, "entry": None}


def parse_cfg(path):
    """Read one -fdump-tree-cfg-blocks-lineno file.

    Returns {function name: {"blocks": {bb: [statement lines]},
    "succs": {bb: [bb]}, "entry": bb}}.  A block's text is kept line by
    line; analyze_block() derives its source lines, names, and gate.
    """
    funcs = {}
    cur = None
    bb = None
    with open(path, errors="replace") as f:
        for ln in f:
            ln = ln.rstrip("\n")
            m = FUNC_RE.match(ln)
            if m:
                cur = funcs.setdefault(m.group(1), new_func())
                bb = None
                continue
            if cur is None:
                continue
            m = SUCC_RE.match(ln)
            if m:
                cur["succs"][int(m.group(1))] = [int(x) for x in m.group(2).split()]
                continue
            m = BB_RE.match(ln)
            if m:
                bb = int(m.group(1))
                cur["blocks"][bb] = []
                continue
            if ln.startswith(";;"):
                if bb is not None and ln.strip() == ";;    pred:       ENTRY":
                    cur["entry"] = bb
                continue
            if ln == "}":
                bb = None
            elif bb is not None and ln.strip():
                cur["blocks"][bb].append(ln)
    return funcs


def analyze_block(text_lines):
    """Derive what reachability needs from one block's statement lines.

    Returns (locs, names, gate): locs is the set of (rel path, line) the
    block's statements carry; names is the set of identifiers it uses
    outside a thread-creating call (R4) and outside string literals and
    member names; gate is (op, true_bb, false_bb) when the block tests a
    simulation gate's result (R3), else None.
    """
    locs = set()
    names = set()
    gate_var = None
    gate = None
    for i, ln in enumerate(text_lines):
        for path, line in LOC_RE.findall(ln):
            rp = rel_path(path)
            if rp is not None:
                locs.add((rp, int(line)))
        body = LOC_RE.sub("", ln)
        m = GATE_CALL_RE.search(body)
        if m:
            gate_var = m.group(1)
        if gate_var is not None and gate is None:
            gate = match_gate(text_lines, i, body, gate_var)
        if THREAD_CREATE.search(body):
            continue
        names.update(IDENT_RE.findall(STRING_RE.sub("", body)))
    return locs, names, gate


def match_gate(text_lines, i, body, var):
    """If line i is `if (var != 0)` / `if (var == 0)` with its two gotos on
    the following lines, return (op, true_bb, false_bb), else None."""
    m = re.search(r"\bif \(%s ([!=])= 0\)" % re.escape(var), body)
    if m is None:
        return None
    rest = " ".join(text_lines[i + 1:i + 4])
    g = re.search(r"goto <bb (\d+)>;.*?\belse\b.*?goto <bb (\d+)>;", rest)
    if g is None:
        return None
    return (m.group(1), int(g.group(1)), int(g.group(2)))


def walk_function(func):
    """Reach the blocks of one function from its entry, honoring R3.

    Returns (reached block set, analyzed blocks by bb, gate count).
    """
    info = {bb: analyze_block(lines) for bb, lines in func["blocks"].items()}
    reached = set()
    stack = [func["entry"]] if func["entry"] is not None else []
    gates = 0
    while stack:
        bb = stack.pop()
        if bb in reached or bb not in info:
            continue
        reached.add(bb)
        succ = func["succs"].get(bb, [])
        gate = info[bb][2]
        if gate is not None:
            op, true_bb, false_bb = gate
            sim_bb, prod_bb = (true_bb, false_bb) if op == "!" else (false_bb, true_bb)
            if prod_bb != sim_bb:
                succ = [s for s in succ if s != prod_bb]
                gates += 1
        stack.extend(succ)
    return reached, info, gates


def map_roots(map_path):
    """The export patterns of every node's global: list in dist/libxtc.map."""
    with open(map_path) as f:
        text = re.sub(r"/\*.*?\*/", "", f.read(), flags=re.S)
    pats = []
    for blk in re.findall(r"global:(.*?)(?:local:|\};)", text, flags=re.S):
        pats.extend(p.strip() for p in blk.split(";") if p.strip())
    return pats


class Library:
    """The parsed dumps of one build directory, and name resolution."""

    def __init__(self, build_dir):
        self.syms = {}
        self.cfgs = {}
        # Only the library's objects: configure's link tests leave dumps
        # (a-conftest.c.*) with no object beside them.
        for path in glob.glob(os.path.join(build_dir, "*.c.*i.cgraph")):
            tu = os.path.basename(path).split(".")[0]
            if os.path.exists(os.path.join(build_dir, tu + ".o")):
                self.syms[tu] = parse_cgraph(path)
        for path in glob.glob(os.path.join(build_dir, "*.c.*t.cfg")):
            tu = os.path.basename(path).split(".")[0]
            if tu in self.syms:
                self.cfgs[tu] = parse_cfg(path)
        self.global_def = {}
        for tu, syms in sorted(self.syms.items()):
            for name, s in syms.items():
                if s["def"] and s["public"]:
                    self.global_def.setdefault((s["kind"], name), tu)

    def resolve(self, tu, name):
        """The (kind, tu, name) a name used in tu refers to, or None for a
        local, a temporary, or a symbol defined outside the library."""
        s = self.syms.get(tu, {}).get(name)
        if s is None:
            return None
        if s["def"] and not s["public"]:
            return (s["kind"], tu, name)
        deftu = self.global_def.get((s["kind"], name))
        return None if deftu is None else (s["kind"], deftu, name)

    def var_functions(self, tu, name, seen):
        """The function keys a variable holds, through nested variables."""
        out = []
        for ref in self.syms[tu][name]["refs"]:
            r = self.resolve(tu, ref)
            if r is None or r in seen:
                continue
            seen.add(r)
            if r[0] == "func":
                out.append((r[1], r[2]))
            else:
                out.extend(self.var_functions(r[1], r[2], seen))
        return out


def compute(build_dir, map_path):
    """Apply R0-R4 to one sim build directory; return the surface record."""
    lib = Library(build_dir)
    pats = map_roots(map_path)
    roots = sorted((tu, name) for (kind, name), tu in lib.global_def.items()
                   if kind == "func" and any(fnmatch.fnmatchcase(name, p) for p in pats))
    reached = {}
    stack = list(roots)
    n_gates = 0
    while stack:
        key = stack.pop()
        if key in reached:
            continue
        func = lib.cfgs.get(key[0], {}).get(key[1])
        if func is None:
            reached[key] = None
            continue
        blocks, info, gates = walk_function(func)
        n_gates += gates
        reached[key] = (blocks, info)
        stack.extend(edges(lib, key[0], blocks, info))
    return record(lib, roots, reached, n_gates, build_dir)


def edges(lib, tu, blocks, info):
    """The function keys the reached blocks of one function lead to (R2)."""
    out = []
    seen = set()
    for bb in blocks:
        for name in info[bb][1]:
            r = lib.resolve(tu, name)
            if r is None or r in seen:
                continue
            seen.add(r)
            if r[0] == "func":
                out.append((r[1], r[2]))
            else:
                out.extend(lib.var_functions(r[1], r[2], seen))
    return out


def record(lib, roots, reached, n_gates, build_dir):
    """Build the JSON record: per TU, its reached functions, and for each
    the lines that only its cut blocks carry."""
    tus = {}
    for (tu, name), val in sorted(reached.items()):
        ent = tus.setdefault(tu, {"functions": [], "cut_lines": {}})
        ent["functions"].append(name)
        if val is None:
            continue
        blocks, info = val
        live = set().union(*(info[b][0] for b in blocks)) if blocks else set()
        dead = set().union(*(info[b][0] for b in info if b not in blocks)) if info else set()
        if dead - live:
            ent["cut_lines"][name] = sorted(dead - live)
    n_defs = sum(1 for syms in lib.syms.values() for s in syms.values()
                 if s["kind"] == "func" and s["def"])
    n_thr = sum(1 for tu in lib.cfgs.values() for fn in tu.values()
                for lines in fn["blocks"].values() for ln in lines
                if THREAD_CREATE.search(LOC_RE.sub("", ln)))
    return {"build": os.path.realpath(build_dir), "configure": configure_line(build_dir),
            "roots": len(roots), "reached": len(reached), "defined": n_defs,
            "gates_cut": n_gates, "thread_calls": n_thr, "tus": tus}


def configure_line(build_dir):
    """The `$ configure ...` line from the build's config.log, for R1."""
    try:
        with open(os.path.join(build_dir, "config.log"), errors="replace") as f:
            for ln in f:
                if ln.startswith("  $ "):
                    return ln[4:].strip()
    except OSError:
        pass
    return "(no config.log)"


def rule_text(rec):
    """The rules as coverage_summary.py prints them, with this run's counts."""
    return [
        "R0 roots: the sim build's global functions that dist/libxtc.map exports"
        " (%d)" % rec["roots"],
        "R1 configuration: %s" % rec["configure"],
        "R2 edges: direct calls, address-taken functions, and functions held by"
        " the initializers of variables used, transitively",
        "R3 __xtc_sim_active()/xtc_sim_active() are true: the not-simulating"
        " side of a gate is cut (%d gate(s) on reached paths)" % rec["gates_cut"],
        "R4 a function passed to a thread-creating call runs on an OS thread"
        " and is cut (%d creating call(s))" % rec["thread_calls"],
        "%d of %d defined functions reached; definition and reasoning:"
        " scripts/dst_surface.py" % (rec["reached"], rec["defined"]),
    ]


FIXTURE_A = r"""
#include <pthread.h>
int __xtc_sim_active(void);
int use(const char *);
int b_public(int);
static void *thr_body(void *a) { return a; }
static int prod_only(int x) { return x + 1; }
static int sim_only(int x) { return x - 1; }
static int cb(int x) { return x * 2; }
static int unused(int x) { return x; }
static int named(int x) { return x; }
struct ops { int (*f)(int); };
static const struct ops tab = { cb };
static const struct ops *const ptab[] = { &tab };
int xtc_api(int x)
{
	pthread_t t;
	if (x == 7)
		(void)pthread_create(&t, 0, thr_body, 0);
	if (!__xtc_sim_active())
		return prod_only(x); /* PROD */
	(void)use("named");
	return sim_only(x) + ptab[0]->f(x) + b_public(x);
}
int helper_not_exported(int x) { return unused(x) + named(x); }
"""

FIXTURE_B = r"""
int b_public(int x) { return x; }
int b_dead(int x) { return x; }
"""


def self_test(cc):
    """Compile the fixture with cc and check R0-R4; return 0 on success."""
    with tempfile.TemporaryDirectory(prefix="dst-surface.") as tmp:
        src = os.path.join(tmp, "src")
        os.mkdir(src)
        for name, text in (("a", FIXTURE_A), ("b", FIXTURE_B)):
            with open(os.path.join(src, name + ".c"), "w") as f:
                f.write(text)
            r = subprocess.run([cc, "-O0", "--coverage", "-fdump-ipa-cgraph",
                                "-fdump-tree-cfg-blocks-lineno", "-c",
                                os.path.join(src, name + ".c"), "-o", name + ".o"],
                               cwd=tmp, capture_output=True, text=True)
            if r.returncode != 0:
                print("dst_surface self-test: %s cannot compile the fixture:\n%s"
                      % (cc, r.stderr), file=sys.stderr)
                return 1
        mp = os.path.join(tmp, "test.map")
        with open(mp, "w") as f:
            f.write("/* xtc_dead; */\nT {\n\tglobal:\n\t\txtc_*;\n\tlocal:\n\t\t*;\n};\n")
        rec = compute(tmp, mp)
    return check_fixture(rec)


def check_fixture(rec):
    """Compare the fixture's surface with what each rule must produce."""
    lines = FIXTURE_A.split("\n")
    prod = next(i for i, ln in enumerate(lines, 1) if "/* PROD */" in ln)
    got = {(tu, fn) for tu, ent in rec["tus"].items() for fn in ent["functions"]}
    want = {("a", "xtc_api"), ("a", "sim_only"), ("a", "cb"), ("b", "b_public")}
    problems = []
    if got != want:
        problems.append("reached %s, want %s" % (sorted(got), sorted(want)))
    a = rec["tus"].get("a", {"cut_lines": {}})
    if ("a.c", prod) not in a["cut_lines"].get("xtc_api", []):
        problems.append("R3 did not cut line %d: %s" % (prod, a["cut_lines"]))
    if rec["roots"] != 1 or rec["thread_calls"] != 1 or rec["gates_cut"] != 1:
        problems.append("counts: roots %d thread_calls %d gates_cut %d, want 1 1 1"
                        % (rec["roots"], rec["thread_calls"], rec["gates_cut"]))
    for p in problems:
        print("dst_surface self-test: FAIL: " + p, file=sys.stderr)
    if not problems:
        print("dst_surface self-test: OK (R0-R4 on the fixture)")
    return 1 if problems else 0


def main():
    """Command line: compute a build's surface, or run the self-test."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true",
                    help="check the rules on a compiled fixture and exit")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc").split()[0],
                    help="the compiler for --self-test (default: $CC or gcc)")
    ap.add_argument("--map", help="dist/libxtc.map (the export patterns, R0)")
    ap.add_argument("--out", help="write the surface record (JSON) here")
    ap.add_argument("build_dir", nargs="?",
                    help="a sim build compiled with -fdump-ipa-cgraph"
                    " -fdump-tree-cfg-blocks-lineno")
    args = ap.parse_args()
    if args.self_test:
        return self_test(args.cc)
    if not (args.map and args.out and args.build_dir):
        ap.error("--map, --out and build_dir are required")
    rec = compute(args.build_dir, args.map)
    if rec["roots"] == 0:
        print("dst_surface: no roots in %s (was it built with the dump flags?)"
              % args.build_dir, file=sys.stderr)
        return 1
    rec["rule"] = rule_text(rec)
    with open(args.out, "w") as f:
        json.dump(rec, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
