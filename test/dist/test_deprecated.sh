#!/bin/sh
# test/dist/test_deprecated.sh -- XTC_DEPRECATED (src/inc/xtc_export.h)
# compiles, warns on use with the given message, and is silenced by
# XTC_NO_DEPRECATION_WARNINGS.  A C++ build checks the [[deprecated]] form
# when a C++ compiler is present.
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
CC="${CC:-cc}"
SRC="$XTC_SRC_DIR"
t="$SRC/test/abi/test_deprecated.c"
work=$(mktemp -d "${TMPDIR:-/tmp}/xtcdep.XXXXXX") || exit 1
trap 'find "$work" -delete 2>/dev/null || true' EXIT INT TERM

$CC -std=c11 -I"$SRC/src/inc" -o "$work/t" "$t" 2> "$work/warn"
"$work/t" >/dev/null
if ! grep -q 'deprecated' "$work/warn" || \
   ! grep -q 'use xtc_abi_new_api' "$work/warn"; then
	echo "  [deprecated] FAIL: a use of an XTC_DEPRECATED function did not" \
		"warn with its message:" >&2
	cat "$work/warn" >&2
	exit 1
fi
$CC -std=c11 -DXTC_NO_DEPRECATION_WARNINGS -I"$SRC/src/inc" \
	-o "$work/t2" "$t" 2> "$work/warn2"
if grep -q 'deprecated' "$work/warn2"; then
	echo "  [deprecated] FAIL: XTC_NO_DEPRECATION_WARNINGS did not silence it" >&2
	exit 1
fi
cxx=""
for c in c++ g++ clang++; do
	if command -v "$c" >/dev/null 2>&1; then cxx=$c; break; fi
done
if [ -n "$cxx" ]; then
	$cxx -std=c++17 -x c++ -I"$SRC/src/inc" -o "$work/t3" "$t" 2> "$work/warn3"
	"$work/t3" >/dev/null
	grep -q 'deprecated' "$work/warn3" || {
		echo "  [deprecated] FAIL: C++ [[deprecated]] form did not warn" >&2
		exit 1
	}
fi
echo "  [deprecated] OK: XTC_DEPRECATED warns with its message (C${cxx:+, C++}), and is silenceable"
