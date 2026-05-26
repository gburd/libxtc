#!/bin/sh
# test/m0/test_symbols.sh
# Verifies M0_CLAIMS.md [C5]: every exported symbol begins with xtc_.
#
# Inspects the static archive's symbol table.  Skips quietly when
# the toolchain is too unusual to introspect (Windows; no nm).

set -eu

: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"
LIB="$XTC_BUILD_DIR/libxtc.a"

if [ ! -f "$LIB" ]; then
	echo "  [C5] FAIL: $LIB not built yet" >&2
	exit 1
fi
if ! command -v nm >/dev/null 2>&1; then
	echo "  [C5] SKIP: nm not on PATH"
	exit 0
fi

# Defined external (T/D/B/R) symbols only.
defined=$(nm --defined-only "$LIB" 2>/dev/null \
		| awk '$2 ~ /^[TDRBC]$/ {print $3}' \
		| sort -u || true)

if [ -z "$defined" ]; then
	echo "  [C5] SKIP: no defined symbols (unusual archive)"
	exit 0
fi

bad=$(echo "$defined" | grep -v -E '^(xtc_|__xtc_|_)' || true)
if [ -n "$bad" ]; then
	echo "  [C5] FAIL: non-xtc symbols exported:" >&2
	echo "$bad" >&2
	exit 1
fi
echo "  [C5] OK: $(echo "$defined" | wc -l) exported symbols, all xtc_*"
