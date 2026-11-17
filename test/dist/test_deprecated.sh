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

# The bridge test (test/abi/test_deprecated_bridge.c) is the one file that
# uses deprecated libxtc names.  Built without XTC_NO_DEPRECATION_WARNINGS,
# every old name in docs/deprecations.md must warn there, except a row
# marked "macro" (only clang can mark a macro); a row marked "frozen" (an
# alias with no warning) must NOT warn.  A missing warning means the old
# name lost its mark, or the bridge test does not use it.
br="$SRC/test/abi/test_deprecated_bridge.c"
tbl="$SRC/docs/deprecations.md"
if [ -n "${XTC_BUILD_DIR:-}" ] && [ -f "$XTC_BUILD_DIR/libxtc.a" ]; then
	LC_ALL=C $CC -std=c11 -D_GNU_SOURCE -Wall -I"$SRC/src/inc" \
		-I"$XTC_BUILD_DIR" -c -o "$work/bridge.o" "$br" 2> "$work/warn4" || {
		echo "  [deprecated] FAIL: the bridge test does not compile" >&2
		cat "$work/warn4" >&2
		exit 1
	}
	# Only clang can mark a macro: compile once more with clang, when
	# there is one, to check the macro rows.
	: > "$work/warn5"
	if command -v clang >/dev/null 2>&1; then
		LC_ALL=C clang -std=c11 -D_GNU_SOURCE -Wall -I"$SRC/src/inc" \
			-I"$XTC_BUILD_DIR" -c -o "$work/bridge5.o" "$br" \
			2> "$work/warn5" || : > "$work/warn5"
		grep -q 'deprecated' "$work/warn5" || : > "$work/warn5"
		[ -s "$work/warn5" ] && CLANG_SEEN=clang
	fi
	# Old-name column (backticks and a trailing () stripped), Kind column.
	sed -n '/<!-- renames:begin -->/,/<!-- renames:end -->/p' "$tbl" |
		awk -F'|' 'NR > 3 && NF > 4 {
			n = $2; gsub(/[` ]/, "", n); sub(/[(][)]$/, "", n)
			k = $4; gsub(/ /, "", k); print n, k }' > "$work/old"
	nold=0
	bad=""
	while read -r o k; do
		nold=$((nold + 1))
		case "$k" in
		*frozen*)
			if grep -qF "'$o' is deprecated" "$work/warn4"; then
				bad="$bad $o(frozen-but-warns)"
			fi ;;
		*macro*)
			[ -s "$work/warn5" ] || continue
			grep -qF "macro '$o' has been marked as deprecated" \
				"$work/warn5" || bad="$bad $o(clang)" ;;
		*)
			grep -qF "'$o' is deprecated" "$work/warn4" ||
				bad="$bad $o" ;;
		esac
	done < "$work/old"
	# Every function row has a deprecated declaration in some installed
	# header's "Deprecated names" block, and each such declaration has a
	# row: the two lists are the same set.
	for h in "$SRC"/src/inc/xtc*.h; do
		sed -n '/Deprecated names (docs\/deprecations.md)/,/^#ifdef __cplusplus/p' "$h"
	done | grep -oE 'xtc_[a-z0-9_]+[(]' | tr -d '(' | sort -u > "$work/decl"
	awk '$2 == "function" { print $1 }' "$work/old" | sort -u > "$work/rows"
	if ! cmp -s "$work/decl" "$work/rows"; then
		echo "  [deprecated] FAIL: header \"Deprecated names\" blocks and the" \
			"docs/deprecations.md function rows differ (< header, > table):" >&2
		diff "$work/decl" "$work/rows" | grep '^[<>]' >&2
		exit 1
	fi
	# Every row has a DEPRECATED entry in a man page and a man3 alias
	# (`man 3 old` must resolve).
	for p in "$SRC"/man/man3/*.3; do
		awk '/^\.S[hH] DEPRECATED/ { f = 1; next } /^\.S[hH] / { f = 0 }
		    f && /^\.(It Fn|It Dv|It Vt|B) / { print $NF }' "$p"
	done | sort -u > "$work/mandep"
	awk '{ print $1 }' "$work/old" | sort -u > "$work/allrows"
	if ! cmp -s "$work/mandep" "$work/allrows"; then
		echo "  [deprecated] FAIL: man DEPRECATED entries and the" \
			"docs/deprecations.md rows differ (< man, > table):" >&2
		diff "$work/mandep" "$work/allrows" | grep '^[<>]' >&2
		exit 1
	fi
	while read -r o; do
		[ -f "$SRC/man/man3/$o.3" ] || bad="$bad $o(no-man3-alias)"
	done < "$work/allrows"
	if [ -n "$bad" ]; then
		echo "  [deprecated] FAIL: docs/deprecations.md rows whose warning" \
			"in test_deprecated_bridge.c is wrong:$bad" >&2
		exit 1
	fi
	echo "  [deprecated] OK: the $nold old names in docs/deprecations.md" \
		"warn in the bridge test (frozen ones do not; macros checked" \
		"with ${CLANG_SEEN:-no clang})"
fi
echo "  [deprecated] OK: XTC_DEPRECATED warns with its message (C${cxx:+, C++}), and is silenceable"
