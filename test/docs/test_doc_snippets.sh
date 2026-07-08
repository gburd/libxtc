#!/bin/sh
# test/docs/test_doc_snippets.sh -- documentation code is a release gate.
#
# Compiles every docs/snippets/*.c against the freshly built static
# library and runs each one that has a main().  A snippet marked
# `/* SNIPPET: compile-only */` on its first line is compiled but not
# run (it is an illustrative fragment, e.g. a signature or a partial).
#
# Every code block shown in the docs/ book is one of these files (or a
# !region of one), so if the public API changes under a snippet, or a
# doc promises behavior a snippet does not actually produce, this fails.
#
# Env:
#   XTC_SRC_DIR  repo root (default: the dir two levels above this script)
#   XTC_BUILD    build dir holding libxtc.a (default: $XTC_SRC_DIR/build_unix)
#   CC           compiler (default: cc)
set -eu

here=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
SRC=${XTC_SRC_DIR:-$(unset CDPATH; cd -- "$here/../.." && pwd)}
BUILD=${XTC_BUILD:-$SRC/build_unix}
CC=${CC:-cc}
LIB="$BUILD/libxtc.a"
INC="$SRC/src/inc"
SNIP="$SRC/docs/_includes/snippets"

if [ ! -f "$LIB" ]; then
	echo "[docs] SKIP: no static library at $LIB (build libxtc first)" >&2
	exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/xtc-docsnip.XXXXXX")
trap 'rm -f "$tmp"/* 2>/dev/null; rmdir "$tmp" 2>/dev/null' EXIT

# Link line mirrors the examples: static lib + pthreads + libm + libdl,
# and liburing only if the library was built against it.
LIBS="-lpthread -lm"
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing 2>/dev/null; then
	if nm "$LIB" 2>/dev/null | grep -q io_uring_queue_init; then
		LIBS="$LIBS $(pkg-config --libs liburing)"
	fi
fi
# libdl is folded into libc on modern glibc; add it only if present.
printf 'int main(void){return 0;}\n' > "$tmp/dltest.c"
if "$CC" "$tmp/dltest.c" -ldl -o "$tmp/dltest" >/dev/null 2>&1; then
	LIBS="$LIBS -ldl"
fi

n=0
fail=0
for f in "$SNIP"/*.c; do
	[ -e "$f" ] || continue
	n=$((n + 1))
	base=$(basename "$f" .c)
	bin="$tmp/$base"
	# $LIBS is a deliberate multi-flag string; word-splitting is intended.
	# shellcheck disable=SC2086
	if ! "$CC" -std=c11 -D_GNU_SOURCE -Wall -Wextra -I"$INC" \
		-o "$bin" "$f" "$LIB" $LIBS >"$tmp/$base.log" 2>&1; then
		echo "[docs] FAIL: $base did not compile" >&2
		sed 's/^/    /' "$tmp/$base.log" >&2
		fail=$((fail + 1))
		continue
	fi
	if head -1 "$f" | grep -q 'SNIPPET: compile-only'; then
		echo "[docs] ok (compile-only): $base"
		continue
	fi
	if ! "$bin" >"$tmp/$base.run" 2>&1; then
		echo "[docs] FAIL: $base compiled but exited non-zero" >&2
		sed 's/^/    /' "$tmp/$base.run" >&2
		fail=$((fail + 1))
		continue
	fi
	echo "[docs] ok: $base"
done

if [ "$fail" -ne 0 ]; then
	echo "[docs] FAIL: $fail of $n documentation snippet(s) broken" >&2
	exit 1
fi
echo "[docs] OK: all $n documentation snippet(s) compile and run"
