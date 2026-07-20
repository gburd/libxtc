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

# [C7]: installed public headers must not DECLARE a __-prefixed function
# (the __ prefix marks a symbol library-internal; a consumer header must
# not expose it).  Exceptions, both principled:
#   - compiler builtins / keywords (__attribute__, __declspec,
#     __has_include, __extension__, __builtin_*) -- not xtc symbols;
#   - a __ symbol a PUBLIC macro in the SAME header expands to (e.g.
#     xtc_proc_recovery_arm() -> __xtc_recovery_prep()): the macro can
#     only expand to a symbol declared in the consumer's TU, so the
#     declaration must live in the installed header.  Allow a __ decl
#     iff its name appears in a #define body in that same header.
# Regressions here are exactly what let __xtc_unsafe_*/__xtc_tail_*/
# __xtc_aio_force_offload leak before they were moved to *_int.h.
c7_bad=""
for h in $hdrs; do
	[ -f "$INC/$h" ] || continue
	# candidate __ decls: a line whose first token (after an optional
	# XTC_API and a return type) begins a __xtc_/__os_ function -- i.e.
	# the identifier is immediately followed by '('.  Strip comments
	# first so prose mentions do not count: awk drops whole-line and
	# block-comment (/* ... */, including ' * ' continuation) content,
	# leaving only real code.
	decls=$(awk '
		{ line = $0 }
		inblock {
			if (line ~ /\*\//) { sub(/.*\*\//, "", line); inblock = 0 }
			else next
		}
		{
			sub(/\/\/.*/, "", line)
			while (line ~ /\/\*.*\*\//) sub(/\/\*.*\*\//, "", line)
			if (line ~ /\/\*/) { sub(/\/\*.*/, "", line); inblock = 1 }
			print line
		}' "$INC/$h" 2>/dev/null \
		| grep -E '(^|[[:space:]*])(__xtc_|__os_)[A-Za-z0-9_]+[[:space:]]*\(' \
		| grep -vE '__attribute__|__declspec|__has_include|__extension__|__builtin_' \
		| grep -oE '(__xtc_|__os_)[A-Za-z0-9_]+' | sort -u)
	# names referenced by a #define body in this header (macro-backed).
	macro_syms=$(grep -E '^[[:space:]]*#[[:space:]]*define' "$INC/$h" 2>/dev/null \
		| grep -oE '(__xtc_|__os_)[A-Za-z0-9_]+' | sort -u)
	# also count symbols used in a continued macro body (backslash lines
	# after a #define) -- approximate by scanning the whole header's
	# define-region names is over-broad; instead accept any __ symbol
	# that appears on a line ending in '\' or immediately after one.
	cont=$(awk '/^[[:space:]]*#[[:space:]]*define/{ind=1}
		     ind{print}
		     ind && !/\\[[:space:]]*$/{ind=0}' "$INC/$h" 2>/dev/null \
		| grep -oE '(__xtc_|__os_)[A-Za-z0-9_]+' | sort -u)
	allowed=$(printf '%s\n%s\n' "$macro_syms" "$cont" | sort -u)
	for d in $decls; do
		echo "$allowed" | grep -qx "$d" || c7_bad="$c7_bad $h:$d"
	done
done

if [ -n "$c7_bad" ]; then
	echo "  [C7] FAIL: installed public headers declare internal __ symbols" >&2
	echo "        (move them to a *_int.h; a __ decl is allowed only when a" >&2
	echo "         public macro in the same header expands to it):" >&2
	for entry in $c7_bad; do echo "    $entry" >&2; done
	exit 1
fi
echo "  [C7] OK: no internal __ symbol declared in an installed public header"

# [C8]: the SHARED library's exported ABI must be exactly the public
# surface -- xtc_* plus only the __ symbols a public macro in an
# installed header expands to (the same allowlist [C7] permits, i.e. the
# recovery-frame seam in xtc_proc.h).  Guards against the version script
# (dist/libxtc.map) drifting back to a broad "__xtc_*" glob that leaks
# ~80 internals into the ABI.  Only meaningful when a shared library was
# built; the static archive legitimately contains every internal, so it
# is out of scope here.
SO=$(find "$XTC_BUILD_DIR" -maxdepth 1 \( -name 'libxtc.so*' -o \
		-name 'libxtc.*.dylib' -o -name 'libxtc.dylib' \) 2>/dev/null \
		| head -1 || true)
if [ -z "$SO" ]; then
	echo "  [C8] SKIP: no shared library built (static-only configure)"
else
	# The allowlist: every __ symbol referenced (in code, not comments)
	# by an installed public header -- computed the same way [C7]
	# derives its exceptions, so the two cannot disagree.  Concatenate
	# the comment-stripped headers first, then grep once, so a single
	# header with no __ reference cannot abort the loop under set -e.
	allow=$(for h in $hdrs; do
			[ -f "$INC/$h" ] || continue
			awk '{l=$0}
				inb{if(l~/\*\//){sub(/.*\*\//,"",l);inb=0}else next}
				{sub(/\/\/.*/,"",l); while(l~/\/\*.*\*\//)sub(/\/\*.*\*\//,"",l); if(l~/\/\*/){sub(/\/\*.*/,"",l);inb=1} print l}' \
				"$INC/$h" 2>/dev/null
		done | grep -oE '(__xtc_|__os_)[A-Za-z0-9_]+' \
			| grep -vE '__attribute__|__declspec|__has_include|__extension__|__builtin_' \
			| sort -u || true)
	# exported dynamic symbols that are internal-looking (__ prefix).
	# ELF nm -D shows __xtc_...; Mach-O prepends one underscore so it
	# shows ___xtc_... (and _xtc_ for public).  Match a __ or ___ prefix
	# and normalize a Mach-O triple-underscore back to the source __.
	exported=$(nm -D --defined-only "$SO" 2>/dev/null \
		| awk '$2 ~ /^[TWDBR]$/ {print $3}' \
		| grep -E '^(__|___)[a-z]' \
		| sed -E 's/^___/__/' \
		| sort -u || true)
	c8_bad=""
	for s in $exported; do
		echo "$allow" | grep -qx "$s" || c8_bad="$c8_bad $s"
	done
	if [ -n "$c8_bad" ]; then
		echo "  [C8] FAIL: shared library exports internal __ symbols not" >&2
		echo "        backed by a public macro (narrow dist/libxtc.map /" >&2
		echo "        the Darwin libxtc.exp filter):" >&2
		for s in $c8_bad; do echo "    $s" >&2; done
		exit 1
	fi
	exp_n=$(echo "$exported" | grep -c '.' || true)
	echo "  [C8] OK: shared library exports xtc_* + $exp_n macro-backed __ symbol(s), no internal leak"
fi
