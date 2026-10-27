#!/bin/sh
# test/dist/test_abi_layout.sh -- the struct-layout gate (2.0 ABI).
#
# Every public struct's sizeof, alignment, and every field's offset and
# size, on this target, must equal the committed table test/abi/layout.txt.
# A layout change is a conscious act: rerun with XTC_ABI_LAYOUT_UPDATE=1
# and commit the new table together with the struct change (and, for a
# struct in the stable tier, a KNOWN_ISSUES / release-note entry saying
# why).  Appending a field to a size-first struct is allowed in a minor;
# moving or resizing an existing field never is.
#
# The table is generated, not hand-written: test/abi/struct_fields.awk
# lists each `typedef struct ... } NAME;` in the installed headers and its
# top-level members; a C probe compiled against the headers prints the
# numbers.  The committed table is the Linux x86-64 one (glibc and musl
# agree: the only libc type embedded in a public struct is xtc_lwlock_t's
# pthread_mutex_t/cond_t, 40/48 bytes on both); on any other OS or data
# model the gate reports SKIP -- the numbers legitimately differ there
# (FreeBSD's pthread_mutex_t is a pointer), and the relative-order rules
# still hold by construction.
#
# Env: XTC_SRC_DIR (repo root), XTC_BUILD_DIR (for xtc_config.h), CC.

set -eu
LC_ALL=C; export LC_ALL   # stable sort order on every host
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"
CC="${CC:-cc}"
SRC="$XTC_SRC_DIR"
INC="$SRC/src/inc"
TABLE="$SRC/test/abi/layout.txt"

work=$(mktemp -d "${TMPDIR:-/tmp}/abilayout.XXXXXX") || exit 1
trap 'find "$work" -delete 2>/dev/null || true' EXIT INT TERM

# Types whose definition is platform-conditional (#if _WIN32 ... #else a
# non-struct typedef): not one layout, so not in the table.
SKIP='xtc_recovery_buf_t'

# The installed header set, from the Makefile (the single source of truth).
hdrs=$(sed -n '/^LIB_HDRS_PUBLIC/,/xtc_trace\.h$/p' "$SRC/dist/Makefile.in" |
	grep -oE 'xtc[a-z0-9_]*\.h' | sort -u)

{
	echo '#include <stddef.h>'
	echo '#include <stdio.h>'
	for h in $hdrs; do echo "#include \"$h\""; done
	# printf %s, not echo: dash's echo would expand the \n (SC2028)
	printf '%s\n' '#define S(T) printf("%s size=%zu align=%zu\n", #T, sizeof(T), _Alignof(T))'
	printf '%s\n' '#define F(T, f) printf("%s.%s off=%zu size=%zu\n", #T, #f, offsetof(T, f), sizeof(((T *)0)->f))'
	echo 'int main(void) {'
	for h in $hdrs; do
		awk -f "$SRC/test/abi/struct_fields.awk" "$INC/$h"
	done | sort -u | grep -vE "^($SKIP):" | while IFS=: read -r t fields; do
		echo "S($t);"
		for f in $fields; do echo "F($t, $f);"; done
	done
	echo 'return 0; }'
} > "$work/probe.c"

if ! $CC -std=c11 -D_DEFAULT_SOURCE -I"$XTC_BUILD_DIR" -I"$INC" \
	-o "$work/probe" "$work/probe.c" 2> "$work/cc.err"; then
	echo "  [abi-layout] FAIL: the layout probe did not compile:" >&2
	head -20 "$work/cc.err" >&2
	exit 1
fi
"$work/probe" > "$work/layout.txt"
n=$(grep -c ' size=.*align=' "$work/layout.txt" || true)

model=$("$CC" -dM -E -x c /dev/null 2>/dev/null |
	grep -cE '#define (__x86_64__|__linux__) ' || true)
if [ "${XTC_ABI_LAYOUT_UPDATE:-0}" = 1 ]; then
	cp "$work/layout.txt" "$TABLE"
	echo "  [abi-layout] UPDATED: $TABLE ($n structs)"
	exit 0
fi
if [ "$model" -lt 2 ]; then
	echo "  [abi-layout] SKIP: table is for Linux x86-64; this target differs"
	exit 0
fi
if [ ! -f "$TABLE" ]; then
	echo "  [abi-layout] FAIL: $TABLE missing (XTC_ABI_LAYOUT_UPDATE=1 to create)" >&2
	exit 1
fi
if ! diff -u "$TABLE" "$work/layout.txt" > "$work/diff"; then
	echo "  [abi-layout] FAIL: public struct layout differs from test/abi/layout.txt:" >&2
	head -40 "$work/diff" >&2
	echo "  A layout change must be deliberate: rerun with" >&2
	echo "  XTC_ABI_LAYOUT_UPDATE=1, commit the table, and record it." >&2
	exit 1
fi
echo "  [abi-layout] OK: $n public structs match test/abi/layout.txt"
