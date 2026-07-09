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

# [C6]: installed public headers must not #define standard or bare
# (non-xtc) macro names.  Guards against regressions like the old
# xtc_bdev.h '#define ssize_t xtc_ssize_t', which silently rewrote the
# ssize_t token in every consumer TU.  Needs the source tree; skip
# cleanly if XTC_SRC_DIR is unset (e.g. run standalone with only a lib).
if [ -z "${XTC_SRC_DIR:-}" ]; then
	echo "  [C6] SKIP: XTC_SRC_DIR not set"
	exit 0
fi

INC="$XTC_SRC_DIR/src/inc"

# Enumerate the INSTALLED public header set from LIB_HDRS_PUBLIC in the
# build's Makefile.in (same parser as test_man_coverage.sh, so the two
# stay in sync).  Internal-only headers are absent from that list and
# thus out of scope.  Fall back to xtc.h + all xtc_*.h if the Makefile
# cannot be parsed.
hdrs=$(awk '
	/^LIB_HDRS_PUBLIC[ \t]*=/ { grab = 1 }
	grab {
		n = split($0, a, /[ \t]+/);
		for (i = 1; i <= n; i++)
			if (a[i] ~ /\/xtc[_A-Za-z0-9]*\.h$/) {
				sub(/.*\//, "", a[i]);
				print a[i];
			}
		if ($0 !~ /\\[ \t]*$/) grab = 0;
	}
' "$XTC_SRC_DIR/dist/Makefile.in" 2>/dev/null | sort -u)
if [ -z "$hdrs" ]; then
	hdrs="xtc.h $(cd "$INC" && ls xtc_*.h 2>/dev/null)"
fi

# A public macro name is allowed iff it is xtc-namespaced (any case:
# XTC_*, xtc_*, _XTC*, __xtc*, __*) or an uppercase include/feature
# guard sentinel (FOO_H, _SSIZE_T_DEFINED, ...).  Anything else -- a
# lowercase or standard identifier like ssize_t/size_t/bool/min -- is a
# namespace leak into the consumer's preprocessor.
c6_bad=""
c6_n=0
for h in $hdrs; do
	[ -f "$INC/$h" ] || continue
	c6_n=$((c6_n + 1))
	names=$(grep -E '^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Za-z_]' \
			"$INC/$h" 2>/dev/null \
		| sed -E 's/^[[:space:]]*#[[:space:]]*define[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/')
	for m in $names; do
		case "$m" in
		XTC_*|xtc_*|_XTC*|_xtc*|__*) ;;
		*_H|*_DEFINED) ;;
		*) c6_bad="$c6_bad $h:$m" ;;
		esac
	done
done

if [ -n "$c6_bad" ]; then
	echo "  [C6] FAIL: public headers define non-xtc/standard macros:" >&2
	for entry in $c6_bad; do echo "    $entry" >&2; done
	exit 1
fi
echo "  [C6] OK: $c6_n public headers, no namespace-polluting macros"
