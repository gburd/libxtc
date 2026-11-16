#!/bin/sh
# test/dist/test_header_alone.sh -- every installed header compiles as
# the only include of a C translation unit.
#
# A header that leans on a type, macro or feature-test macro its own
# includes do not provide compiles in the library (xtc_int.h sets them
# up first) and breaks a consumer whose include order differs.  This
# compiles each header in LIB_HDRS_PUBLIC (dist/Makefile.in) alone, as C,
# with gcc and clang where present, -Wall -Wextra -Wpedantic -Werror:
#
#   gnu11                              every header must compile
#   c11 -D_POSIX_C_SOURCE=200809L      every header must compile
#   c11 (strict ISO C, no POSIX)       every header must compile except
#                                      those in STRICT_ALLOW below
#
# STRICT_ALLOW is the set that reaches sigjmp_buf (POSIX, not ISO C)
# through xtc_proc.h's recovery-frame API.  It goes away when that API
# moves to its own header (API plan B5); until then the list is exact:
# a header that fails and is not on it fails the gate, and so does one
# on it that compiles, so the list cannot go stale.
#
# The C++ side (each header alone with g++ and clang++) is
# test/dist/test_header_cxx.sh.
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
INC="$XTC_SRC_DIR/src/inc"
LC_ALL=C
export LC_ALL
STRICT_ALLOW="xtc_alloc_audit.h xtc_app.h xtc_fsm.h xtc_inspect.h
xtc_launch.h xtc_orc.h xtc_pg.h xtc_proc.h xtc_reg.h xtc_slab.h xtc_svr.h
xtc_tail.h xtc_trace.h xtc_xproc.h"

work=$(mktemp -d "${TMPDIR:-/tmp}/xtchdralone.XXXXXX") || exit 1
trap 'find "$work" -delete 2>/dev/null || true' EXIT INT TERM

hdrs=$(awk '
	/^LIB_HDRS_PUBLIC[ \t]*=/ { grab = 1 }
	grab {
		n = split($0, a, /[ \t]+/)
		for (i = 1; i <= n; i++)
			if (a[i] ~ /\/xtc[_A-Za-z0-9]*\.h$/) {
				sub(/.*\//, "", a[i])
				print a[i]
			}
		if ($0 !~ /\\[ \t]*$/) grab = 0
	}
' "$XTC_SRC_DIR/dist/Makefile.in")
if [ -z "$hdrs" ]; then
	echo "  [header-alone] FAIL: no LIB_HDRS_PUBLIC in dist/Makefile.in" >&2
	exit 1
fi

fail=0
ran=""
skipped=""
# Compile every header alone with compiler $1 and flags $2; print the
# names of those that fail, one per line.
failing() {
	for h in $hdrs; do
		# A declaration after the include: ISO C forbids an empty
		# unit, and xtc_export.h alone declares nothing.
		printf '#include "%s"\nint xtc_header_alone_probe;\n' "$h" \
		    > "$work/one.c"
		# shellcheck disable=SC2086
		"$1" $2 -Wall -Wextra -Wpedantic -Werror -I"$INC" -c \
		    -o "$work/one.o" "$work/one.c" > "$work/one.err" 2>&1 || {
			echo "$h"
			sed 's/^/        /' "$work/one.err" | head -4 \
			    >> "$work/why"
		}
	done
}

for cc in gcc clang; do
	if ! command -v "$cc" >/dev/null 2>&1; then
		skipped="$skipped $cc"
		continue
	fi
	for mode in "-std=gnu11" "-std=c11 -D_POSIX_C_SOURCE=200809L"; do
		: > "$work/why"
		bad=$(failing "$cc" "$mode")
		if [ -n "$bad" ]; then
			echo "  [header-alone] FAIL: $cc $mode:" \
			    "does not compile alone:" $bad >&2
			cat "$work/why" >&2
			fail=1
		fi
		ran="$ran $cc($mode)"
	done
	: > "$work/why"
	bad=$(failing "$cc" "-std=c11" | sort)
	want=$(printf '%s\n' $STRICT_ALLOW | sort)
	new=$(printf '%s\n' "$bad" | grep -vxF "$want" | grep . || true)
	gone=$(printf '%s\n' "$want" | grep -vxF "$bad" | grep . || true)
	if [ -n "$new" ]; then
		echo "  [header-alone] FAIL: $cc -std=c11: does not compile" \
		    "alone and is not in STRICT_ALLOW:" $new >&2
		cat "$work/why" >&2
		fail=1
	fi
	if [ -n "$gone" ]; then
		echo "  [header-alone] FAIL: $cc -std=c11: now compiles alone;" \
		    "drop it from STRICT_ALLOW:" $gone >&2
		fail=1
	fi
	ran="$ran $cc(-std=c11, $(printf '%s\n' $STRICT_ALLOW | grep -c .) allowed)"
done
[ "$fail" -eq 0 ] || exit 1
if [ -z "$ran" ]; then
	echo "  [header-alone] SKIP: neither gcc nor clang found"
	exit 0
fi
n=$(printf '%s\n' $hdrs | grep -c .)
echo "  [header-alone] OK: $n installed headers compile alone:$ran${skipped:+ (SKIP:$skipped)}"
