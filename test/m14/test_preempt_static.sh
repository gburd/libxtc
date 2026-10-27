#!/bin/sh
# test/m14/test_preempt_static.sh -- involuntary preemption trusts no
# code it cannot vouch for (src/ptc/preempt.c, SAFE POINTS).  A program
# that links libxtc.a (or a static libc) cannot have its own code told
# apart from libxtc's or libc's by segment, so only XTC_PREEMPTIBLE code
# and added regions are preemptible there:
#   [P1] libxtc.a, no XTC_PREEMPTIBLE function, no region: enabling fails
#        with XTC_E_NOSYS and says why on stderr.
#   [P2] the same program after xtc_preempt_region_add: enabling works.
#   [P3] libxtc.a with an XTC_PREEMPTIBLE function: enabling works.
#   [P4] a fully static executable (static libc too), no mark: refused.
#   [P5] a shared libxtc (when --enable-shared built one): the main
#        executable's code is preemptible by default, no mark needed.
# Linux x86-64 / aarch64 only (where the redirect exists); [P4] skips
# without a static libc, [P5] without libxtc.so.
# Env: XTC_BUILD_DIR (holds libxtc.a), XTC_SRC_DIR, CC, and the build's
# XTC_SNIPPET_CFLAGS / XTC_SNIPPET_LDFLAGS (sanitizer builds need them).
set -u
B=${XTC_BUILD_DIR:-.}
S=${XTC_SRC_DIR:-..}
CC=${CC:-cc}
SCF=${XTC_SNIPPET_CFLAGS:-}
SLF=${XTC_SNIPPET_LDFLAGS:-}
case "$SCF" in
*-fsanitize=*)
	# Under a sanitizer the redirect is not exercised and a -static link
	# is not possible; the involuntary tests skip there too.
	echo "[pstatic] SKIP: sanitizer build"
	exit 0 ;;
esac
case "$(uname -s)-$(uname -m)" in
Linux-x86_64 | Linux-aarch64) ;;
*) echo "[pstatic] SKIP: no involuntary redirect on $(uname -s)-$(uname -m)"
   exit 0 ;;
esac
fail=0
T=${TMPDIR:-/tmp}/xtc_pstatic.$$
mkdir -p "$T" || exit 1
trap 'rm -f "$T"/*; rmdir "$T" 2>/dev/null' EXIT INT TERM

cat >"$T/p.c" <<'PROBE'
#include <stdio.h>
#include "xtc.h"
#include "xtc_preempt_involuntary.h"
#if defined(WITH_MARK)
XTC_PREEMPTIBLE int marked(int x);
XTC_PREEMPTIBLE int marked(int x) { return x * 3 + 1; }
#endif
static unsigned char region[64];
int
main(int argc, char **argv)
{
	int a, b, c;
	(void)argv;
#if defined(WITH_MARK)
	if (marked(argc) == 0)
		return 9;
#else
	(void)argc;
#endif
	if (!xtc_preempt_involuntary_supported()) {
		printf("unsupported\n");
		return 0;
	}
	a = xtc_preempt_set_involuntary(1);
	(void)xtc_preempt_set_involuntary(0);
	b = xtc_preempt_region_add(region, region + sizeof region);
	c = xtc_preempt_set_involuntary(1);
	printf("enable=%d add=%d enable_after_add=%d\n", a, b, c);
	return 0;
}
PROBE
LIBS=$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' "$B/Makefile")
build() {   # build OUT EXTRA-FLAGS...
	o=$1
	shift
	# shellcheck disable=SC2086  # LIBS and the snippet flags are lists
	$CC -O1 $SCF "$@" -I"$B" -I"$S/src/inc" -o "$T/$o" "$T/p.c" \
	    "$B/libxtc.a" $SLF -pthread $LIBS >"$T/cc.log" 2>&1
}
if ! build p; then
	echo "[pstatic] FAIL: could not build the probe: $(head -3 "$T/cc.log")"
	exit 1
fi
out=$("$T/p" 2>"$T/err")
case "$out" in
unsupported)
	echo "[pstatic] SKIP: this build has no involuntary redirect"
	exit 0 ;;
"enable=-3 add=0 enable_after_add=0")
	if grep -q "libxtc is linked statically" "$T/err"; then
		echo "[pstatic] OK [P1]: static libxtc, no mark: XTC_E_NOSYS, reported"
		echo "[pstatic] OK [P2]: after xtc_preempt_region_add: enabled"
	else
		echo "[pstatic] FAIL [P1]: refused but did not say why: $(cat "$T/err")"
		fail=1
	fi ;;
*)	echo "[pstatic] FAIL [P1/P2]: got '$out' (want enable=-3 add=0 enable_after_add=0)"
	fail=1 ;;
esac
if build pm -DWITH_MARK; then
	out=$("$T/pm" 2>/dev/null)
	case "$out" in
	"enable=0 "*) echo "[pstatic] OK [P3]: static libxtc with an XTC_PREEMPTIBLE function: enabled" ;;
	*) echo "[pstatic] FAIL [P3]: got '$out' (want enable=0)"; fail=1 ;;
	esac
else
	echo "[pstatic] FAIL [P3]: the WITH_MARK build failed"
	fail=1
fi
if build ps -static; then
	out=$("$T/ps" 2>/dev/null)
	case "$out" in
	"enable=-3 "*) echo "[pstatic] OK [P4]: fully static, no mark: refused" ;;
	*) echo "[pstatic] FAIL [P4]: got '$out' (want enable=-3)"; fail=1 ;;
	esac
else
	echo "[pstatic] SKIP [P4]: no static link here"
fi
so=$(find "$B" -maxdepth 1 -name 'libxtc.so.*' 2>/dev/null | head -1)
if [ -n "$so" ] && $CC -O1 -I"$B" -I"$S/src/inc" -o "$T/pd" "$T/p.c" \
    -L"$B" -lxtc -Wl,-rpath,"$B" -pthread >"$T/cc.log" 2>&1; then
	out=$("$T/pd" 2>/dev/null)
	case "$out" in
	"enable=0 "*) echo "[pstatic] OK [P5]: shared libxtc: the main executable is preemptible by default" ;;
	*) echo "[pstatic] FAIL [P5]: got '$out' (want enable=0)"; fail=1 ;;
	esac
else
	echo "[pstatic] SKIP [P5]: no libxtc.so in $B"
fi
exit $fail
