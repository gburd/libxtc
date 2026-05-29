#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# dist/mkamalgamation.py
#
#       Generate the single-file amalgamation: xtc.h (the full public
#       API) and xtc.c (the entire library in one translation unit),
#       SQLite-style, so a project can drop the two files into its tree
#       and build xtc with no separate library.
#
#       Algorithm: depth-first inline of local ("...") includes, with a
#       per-guard "already emitted" set so each header appears once;
#       system (<...>) includes are hoisted, de-duplicated, to the top.
#       Public headers form xtc.h; the internal headers plus every
#       library .c form xtc.c.  Exactly one coroutine substrate
#       (ucontext) is included -- the portable drop-in default.
#
#       Version block: parsed from dist/version.in plus git (exact tag,
#       nearest tag, short and long commit hash, and an optional
#       qualifier such as -rc1).
#
#       Debug #line remapping: by default the amalgamation reports its
#       own file/line.  When built with -DDEBUG and
#       -DXTC_RELATIVE_LOC=<path-from-amalgamation-to-libxtc-root>, each
#       original file's body is prefixed with a #line directive that
#       points debug info at the original source under XTC_RELATIVE_LOC
#       (e.g. a git submodule at ../contrib/libxtc), not at the
#       amalgamation.  The path is built with token-paste stringize
#       (#line N XTC__STR(XTC_RELATIVE_LOC/rel/path)), which gcc and
#       clang accept as a single string literal.

import argparse
import os
import re
import subprocess
import sys

INC_LOCAL = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"')
INC_SYS   = re.compile(r'^[ \t]*#[ \t]*include[ \t]+<([^>]+)>')


def git(root, *args):
    try:
        out = subprocess.check_output(["git", *args], cwd=root,
                                      stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except Exception:
        return ""


def find_header(name, search_dirs):
    for d in search_dirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


class Amalgamator:
    def __init__(self, root):
        self.root = root
        self.incdir = os.path.join(root, "src", "inc")
        self.search = [self.incdir]
        self.emitted = set()      # local header basenames already inlined
        self.sysincs = []         # ordered, de-duplicated <...> includes
        self.sysseen = set()

    def relpath(self, path):
        return os.path.relpath(os.path.abspath(path), self.root)

    def _line_directive(self, path):
        # Under the debug-remap macro, point this file's body at the
        # original source beneath XTC_RELATIVE_LOC.  When the macro is
        # off (the default), emit nothing: the body keeps the
        # amalgamation's own physical line numbering, so diagnostics
        # and debug info refer to xtc.c -- the file the consumer ships.
        rel = self.relpath(path)
        return ("#ifdef XTC__AMALG_REMAP\n"
                "#line 1 XTC__STR(XTC_RELATIVE_LOC/%s)\n"
                "#endif\n" % rel)

    def inline(self, path, out, follow_local=True):
        """Append `path`'s body to out[], inlining local includes
        depth-first and hoisting system includes.  Each header is
        emitted at most once (keyed by basename)."""
        base = os.path.basename(path)
        if base in self.emitted:
            return
        self.emitted.add(base)

        body = []
        with open(path) as f:
            for raw in f:
                m = INC_LOCAL.match(raw)
                if m:
                    dep = find_header(m.group(1), self.search)
                    if dep is not None and follow_local:
                        # Inline the dependency first (its content lands
                        # before ours), then drop this #include line.
                        self.inline(dep, out)
                        continue
                    if dep is not None:
                        continue   # known local header: drop, inlined elsewhere
                    # Unknown local include: keep verbatim.
                    body.append(raw)
                    continue
                ms = INC_SYS.match(raw)
                if ms:
                    # Keep system includes in place: hoisting them would
                    # pull conditionally-guarded ones (<intrin.h> under
                    # _MSC_VER, <windows.h> under _WIN32, ...) out of
                    # their #if context.  Duplicates are harmless thanks
                    # to the headers' own include guards.
                    body.append(raw)
                    continue
                body.append(raw)

        out.append(self._line_directive(path))
        out.extend(body)
        # Under remap, the next inlined file's own #line re-anchors
        # attribution; in the default build no #line was emitted, so
        # physical line numbering of the amalgamation simply continues.


def version_defines(root, qualifier):
    ver = open(os.path.join(root, "dist", "version.in")).read().strip()
    m = re.match(r"(\d+)\.(\d+)\.(\d+)(.*)", ver)
    if not m:
        major = minor = patch = "0"
        extra = ""
    else:
        major, minor, patch, extra = m.groups()
    qual = qualifier if qualifier else extra.lstrip("-")
    short = git(root, "rev-parse", "--short", "HEAD")
    longh = git(root, "rev-parse", "HEAD")
    exact = git(root, "describe", "--tags", "--exact-match")
    nearest = git(root, "describe", "--tags", "--abbrev=0")
    full = "%s.%s.%s%s" % (major, minor, patch,
                           ("-" + qual) if qual else "")
    L = []
    L.append("/* --- amalgamation version identity (generated) --- */\n")
    L.append("#define XTC_VERSION_MAJOR %s\n" % major)
    L.append("#define XTC_VERSION_MINOR %s\n" % minor)
    L.append("#define XTC_VERSION_PATCH %s\n" % patch)
    L.append("#define XTC_VERSION_QUALIFIER \"%s\"\n" % qual)
    L.append("#define XTC_VERSION_STRING \"%s\"\n" % full)
    L.append("#define XTC_VERSION_COMMIT_SHORT \"%s\"\n" % short)
    L.append("#define XTC_VERSION_COMMIT_LONG \"%s\"\n" % longh)
    L.append("#define XTC_VERSION_TAG \"%s\"\n" % exact)
    L.append("#define XTC_VERSION_TAG_NEAREST \"%s\"\n" % nearest)
    L.append("#define XTC_AMALGAMATION 1\n")
    return "".join(L), full, short


REMAP_PROLOGUE = """\
/* Debug #line remapping (see dist/mkamalgamation.py header comment).
 * Build with -DDEBUG -DXTC_RELATIVE_LOC=<rel-path-to-libxtc-root> to
 * point debug info at the original sources instead of this file. */
#if defined(DEBUG) && defined(XTC_RELATIVE_LOC)
# define XTC__STR2(x) #x
# define XTC__STR(x) XTC__STR2(x)
# define XTC__AMALG_REMAP 1
#endif

/* I/O backend selection.  The non-amalgamated build gets this from
 * configure (xtc_config.h); the drop-in amalgamation picks a sensible
 * default per platform unless the consumer pre-defines one.  Override
 * by compiling with e.g. -DXTC_IO_BACKEND_EPOLL. */
#if !defined(XTC_IO_BACKEND_POLL) && !defined(XTC_IO_BACKEND_EPOLL) && \\
    !defined(XTC_IO_BACKEND_URING) && !defined(XTC_IO_BACKEND_KQUEUE) && \\
    !defined(XTC_IO_BACKEND_IOCP) && !defined(XTC_IO_BACKEND_SOLARIS) && \\
    !defined(XTC_IO_BACKEND_AIX) && !defined(XTC_IO_BACKEND_SELECT)
# if defined(__linux__)
#  define XTC_IO_BACKEND_EPOLL 1
# elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \\
       defined(__OpenBSD__) || defined(__DragonFly__)
#  define XTC_IO_BACKEND_KQUEUE 1
# elif defined(_WIN32)
#  define XTC_IO_BACKEND_IOCP 1
# elif defined(__sun)
#  define XTC_IO_BACKEND_SOLARIS 1
# elif defined(_AIX)
#  define XTC_IO_BACKEND_AIX 1
# else
#  define XTC_IO_BACKEND_POLL 1
# endif
#endif

/* ucontext is the amalgamation's coroutine substrate (coro_uctx.c).
 * Define it unless the consumer is on a platform we know lacks it. */
#if !defined(XTC_HAVE_UCONTEXT) && !defined(_WIN32) && !defined(XTC_NO_UCONTEXT)
# define XTC_HAVE_UCONTEXT 1
#endif
"""


def public_headers(incdir):
    # Public API headers: xtc_*.h, excluding the internal umbrella
    # (xtc_int.h) and the generated symbol-list stub (xtc_ext.h).
    names = sorted(n for n in os.listdir(incdir)
                   if n.startswith("xtc_") and n.endswith(".h")
                   and n not in ("xtc_int.h", "xtc_ext.h"))
    # xtc.h (the umbrella) goes first if present.
    out = []
    if os.path.isfile(os.path.join(incdir, "xtc.h")):
        out.append("xtc.h")
    out.extend(names)
    return out


def lib_sources(root):
    # Parse LIB_SRCS from dist/Makefile.in, keep .c (drop .S asm: the
    # amalgamation is C-only and uses the ucontext substrate), and drop
    # the non-ucontext coroutine substrates so exactly one is included.
    mk = open(os.path.join(root, "dist", "Makefile.in")).read()
    m = re.search(r"LIB_SRCS\s*=(.*?)\n\n", mk, re.S)
    srcs = re.findall(r"src/[^\s\\]+\.c", m.group(1))
    keep = []
    for s in srcs:
        b = os.path.basename(s)
        if b in ("coro_fctx.c", "coro_winfiber.c"):
            continue   # amalgamation uses coro_uctx.c (ucontext)
        keep.append(os.path.join(root, s))
    return keep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--out", default=None,
                    help="output directory (default: <root>/amalgamation)")
    ap.add_argument("--qualifier", default="",
                    help="version qualifier, e.g. rc1")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    outdir = args.out or os.path.join(root, "amalgamation")
    os.makedirs(outdir, exist_ok=True)
    incdir = os.path.join(root, "src", "inc")

    vdefs, vfull, vshort = version_defines(root, args.qualifier)

    # ---- xtc.h ----
    amh = Amalgamator(root)
    body = []
    for h in public_headers(incdir):
        amh.inline(os.path.join(incdir, h), body)
    hdr = []
    hdr.append("/* xtc.h -- libxtc %s (%s) amalgamated public API.\n"
               " * Generated by dist/mkamalgamation.py; do not edit.\n"
               " */\n" % (vfull, vshort))
    hdr.append("#ifndef XTC_AMALGAMATION_H\n#define XTC_AMALGAMATION_H\n\n")
    hdr.append(REMAP_PROLOGUE + "\n")
    hdr.append(vdefs + "\n")
    hdr.append("".join(amh.sysincs) + "\n")
    hdr.append("".join(body))
    hdr.append("\n#endif /* XTC_AMALGAMATION_H */\n")
    with open(os.path.join(outdir, "xtc.h"), "w") as f:
        f.write("".join(hdr))

    # ---- xtc.c ----
    amc = Amalgamator(root)
    # Pretend the public headers are already emitted: xtc.c includes
    # xtc.h, so we must not re-inline them here.  Seed the emitted set.
    for h in public_headers(incdir):
        amc.emitted.add(os.path.basename(h))
    cbody = []
    srcs = lib_sources(root)
    for s in srcs:
        amc.inline(s, cbody)     # internal headers get inlined on demand
    cf = []
    cf.append("/* xtc.c -- libxtc %s (%s) amalgamated implementation.\n"
              " * Generated by dist/mkamalgamation.py; do not edit.\n"
              " */\n" % (vfull, vshort))
    cf.append("#include \"xtc.h\"\n\n")
    # System includes the .c bodies need, hoisted + de-duplicated.
    cf.append("".join(amc.sysincs) + "\n")
    cf.append("".join(cbody))
    with open(os.path.join(outdir, "xtc.c"), "w") as f:
        f.write("".join(cf))

    print("wrote %s/xtc.h and %s/xtc.c (version %s, commit %s)"
          % (outdir, outdir, vfull, vshort))
    print("  sources: %d .c files; public headers: %d"
          % (len(srcs), len(public_headers(incdir))))


if __name__ == "__main__":
    main()
