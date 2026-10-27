#!/bin/sh
# test/m14/test_preempt_static.sh -- involuntary preemption trusts no
# code it cannot vouch for (src/ptc/preempt.c, SAFE POINTS):
#   [P1] libxtc.a's text lives in the xtc_text section (the build's
#        objcopy rename): no executable .text* section is left, so a
#        static libxtc inside the main executable is still excluded.
#   [P2] a STATIC executable (libc inside the main text) with no
#        XTC_PREEMPTIBLE function and no added region: enabling fails
#        with XTC_E_NOSYS and says why on stderr.
#   [P3] the same program after xtc_preempt_region_add: enabling works.
#   [P4] a static executable with an XTC_PREEMPTIBLE function: enabling
#        works without any region added.
# Linux x86-64 / aarch64 only (where the redirect exists); [P2]-[P4]
# SKIP without a static libc.
# Env: XTC_BUILD_DIR (holds libxtc.a), XTC_SRC_DIR, CC.
set -u
B=${XTC_BUILD_DIR:-.}
S=${XTC_SRC_DIR:-..}
CC=${CC:-cc}
case "$(uname -s)-$(uname -m)" in
Linux-x86_64 | Linux-aarch64) ;;
*) echo "[pstatic] SKIP: no involuntary redirect on $(uname -s)-$(uname -m)"
   exit 0 ;;
esac
if ! grep -q '^XTC_OBJCOPY[[:space:]]*=[[:space:]]*[^[:space:]]' "$B/Makefile" 2>/dev/null; then
	echo "[pstatic] SKIP: configure found no objcopy"
	exit 0
fi
fail=0
T=${TMPDIR:-/tmp}/xtc_pstatic.$$
mkdir -p "$T" || exit 1
trap 'rm -f "$T"/*; rmdir "$T" 2>/dev/null' EXIT INT TERM

# [P1]
if command -v readelf >/dev/null 2>&1; then
	left=$(readelf -SW "$B/libxtc.a" 2>/dev/null |
	    sed -n 's/^ *\[ *[0-9]*\] *\([^ ]*\) .* AX .*/\1/p' |
	    grep -v '^xtc_text$' | sort -u | tr '\n' ' ')
	if [ -n "$left" ]; then
		echo "[pstatic] FAIL [P1]: libxtc.a has executable sections outside xtc_text: $left"
		fail=1
	else
		echo "[pstatic] OK [P1]: libxtc.a's text is all in xtc_text"
	fi
else
	echo "[pstatic] SKIP [P1]: no readelf"
fi

cat >"$T/p.c" <<'EOF'
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
	a = xtc_preempt_set_involuntary(1);
	(void)xtc_preempt_set_involuntary(0);
	b = xtc_preempt_region_add(region, region + sizeof region);
	c = xtc_preempt_set_involuntary(1);
	printf("enable=%d add=%d enable_after_add=%d\n", a, b, c);
	return 0;
}
EOF
LIBS=$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' "$B/Makefile")
if ! $CC -static -O1 -I"$B" -I"$S/src/inc" -o "$T/p" "$T/p.c" \
    "$B/libxtc.a" -pthread $LIBS >"$T/cc.log" 2>&1; then
	echo "[pstatic] SKIP [P2-P4]: no static link here ($(grep -m1 -iE 'cannot find|error' "$T/cc.log"))"
	exit $fail
fi
out=$("$T/p" 2>"$T/err")
case "$out" in
"enable=-3 add=0 enable_after_add=0")
	if grep -q "involuntary preemption stays off: static executable" "$T/err"; then
		echo "[pstatic] OK [P2]: static, no range: XTC_E_NOSYS, reported"
		echo "[pstatic] OK [P3]: after xtc_preempt_region_add: enabled"
	else
		echo "[pstatic] FAIL [P2]: refused but did not say why: $(cat "$T/err")"
		fail=1
	fi ;;
*)	echo "[pstatic] FAIL [P2/P3]: got '$out' (want enable=-3 add=0 enable_after_add=0)"
	fail=1 ;;
esac
if $CC -static -O1 -DWITH_MARK -I"$B" -I"$S/src/inc" -o "$T/pm" "$T/p.c" \
    "$B/libxtc.a" -pthread $LIBS >"$T/cc.log" 2>&1; then
	out=$("$T/pm" 2>/dev/null)
	case "$out" in
	"enable=0 "*) echo "[pstatic] OK [P4]: static with an XTC_PREEMPTIBLE function: enabled" ;;
	*) echo "[pstatic] FAIL [P4]: got '$out' (want enable=0)"; fail=1 ;;
	esac
else
	echo "[pstatic] FAIL [P4]: the WITH_MARK build failed"
	fail=1
fi
exit $fail
