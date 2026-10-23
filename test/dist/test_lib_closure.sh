#!/bin/sh
# test/dist/test_lib_closure.sh -- the built libxtc.a must be closed over
# its own internal symbols: every __xtc_* / __os_* that one archive member
# references must be DEFINED by another member.
#
# Why: a reference with no definition links fine into a static program
# that never pulls the referencing member, so make check stays green,
# but it breaks every consumer of libxtc.so outright ("undefined symbol").
# That shipped once: the x86-64 preemption trampoline (assembled on every
# x86-64 ELF platform) called __xtc_preempt_take_pc, which was defined
# only in the Linux+ucontext branch of coro_uctx.c, so on FreeBSD (and on
# a Linux fctx build) nothing could link against the shared library.
#
# External names (libc, liburing, the TLS library) are out of scope: the
# check is restricted to the library's own internal namespaces.
#
# Env: XTC_BUILD_DIR (the build tree holding libxtc.a; default .).
set -u
B=${XTC_BUILD_DIR:-.}
A=$B/libxtc.a
NM=${NM:-nm}
if [ ! -f "$A" ]; then
	echo "[closure] SKIP: no $A"
	exit 0
fi
if ! command -v "$NM" >/dev/null 2>&1; then
	echo "[closure] SKIP: no nm"
	exit 0
fi
T=${TMPDIR:-/tmp}/xtc_closure.$$
trap 'rm -f "$T.u" "$T.d"' EXIT INT TERM
# -P (POSIX format): "name type [value size]", one symbol per line.
"$NM" -P "$A" 2>/dev/null | awk '$2 == "U" { print $1 }' | sort -u >"$T.u"
"$NM" -P "$A" 2>/dev/null | awk 'NF >= 2 && $2 != "U" && $2 != ":" { print $1 }' |
    sort -u >"$T.d"
if [ ! -s "$T.d" ]; then
	echo "[closure] SKIP: nm -P produced no symbols for $A"
	exit 0
fi
# Mach-O prefixes every C name with an underscore; match both spellings.
missing=$(comm -23 "$T.u" "$T.d" | grep -E '^_?__(xtc|os)_' || true)
if [ -n "$missing" ]; then
	echo "[closure] FAIL: libxtc.a references internal symbols it never defines" >&2
	echo "          (a libxtc.so built from these objects cannot be linked):" >&2
	echo "$missing" | sed 's/^/            /' >&2
	exit 1
fi
echo "[closure] OK: every internal __xtc_*/__os_* reference in libxtc.a is defined"
exit 0
