#!/bin/sh
# test/dist/test_header_api.sh -- every function an installed header
# declares carries XTC_API.
#
# XTC_API is __declspec(dllexport) while the library builds as a Windows
# DLL and __declspec(dllimport) in a consumer that links one
# (xtc_export.h).  A declaration without it is not exported from the DLL,
# so the consumer fails to link, and nothing on an ELF or Mach-O build
# notices: XTC_API is empty there.  This gate makes the omission visible
# on every host.  It preprocesses the installed headers (LIB_HDRS_PUBLIC
# in dist/Makefile.in) with XTC_API defined as a marker attribute and has
# the compiler print every declaration back (clang -ast-print), so a
# declaration is checked whatever its formatting or line breaks.  Each
# platform branch of the headers is checked: the host's, and _WIN32,
# __APPLE__ and __FreeBSD__ (a stub <windows.h> supplies the two names
# xtc_proc.h takes from it).  static inline functions are out of scope:
# they are compiled into the consumer.
#
# Needs clang; without it the gate is a SKIP, said by name.
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
INC="$XTC_SRC_DIR/src/inc"
CLANG=${CLANG:-clang}
LC_ALL=C
export LC_ALL

if ! command -v "$CLANG" >/dev/null 2>&1; then
	echo "  [header-api] SKIP: $CLANG not found"
	exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/xtchdrapi.XXXXXX") || exit 1
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
	echo "  [header-api] FAIL: no LIB_HDRS_PUBLIC in dist/Makefile.in" >&2
	exit 1
fi
for h in $hdrs; do
	echo "#include \"$h\""
done > "$work/all.c"
mkdir "$work/win"
printf '%s\n' 'typedef struct _CONTEXT { int x; } CONTEXT;' \
    'void RtlCaptureContext(CONTEXT *);' > "$work/win/windows.h"

# Print the declarations of one platform branch: every top-level line the
# compiler prints back that declares an xtc_ / __xtc_ function.  clang 19
# and later print a leading attribute before the return type, clang 18 and
# earlier after the declarator; the sed moves it behind, so one awk serves.  Defining
# XTC_EXPORT_H first keeps xtc_export.h from redefining the marker.
decls() {
	"$CLANG" -std=gnu11 -fsyntax-only -Xclang -ast-print \
	    -DXTC_EXPORT_H '-DXTC_API=__attribute__((annotate("xtc_api")))' \
	    '-DXTC_DEPRECATED(m)=' "$@" -I"$INC" "$work/all.c" \
	    2> "$work/err" |
	    sed 's/^__attribute__((annotate("xtc_api"))) \(.*\)$/\1 __attribute__((annotate("xtc_api")))/' |
	    awk '/^[^ \t}#][^(]*[ *]_*xtc_[A-Za-z0-9_]+\(/ &&
	        !/^static/ && !/^typedef/'
}

fail=0
ran=""
for plat in host _WIN32 __APPLE__ __FreeBSD__; do
	case $plat in
	host)	set -- ;;
	_WIN32)	set -- -U__linux__ -U__unix__ -D_WIN32 -isystem "$work/win" ;;
	*)	set -- -U__linux__ "-D$plat" ;;
	esac
	if ! decls "$@" > "$work/decls"; then
		echo "  [header-api] FAIL: $plat: the headers do not compile:" >&2
		grep -v warning "$work/err" | head -10 | sed 's/^/        /' >&2
		fail=1
		continue
	fi
	n=$(grep -c . "$work/decls" || true)
	missing=$(grep -v 'annotate("xtc_api")' "$work/decls" |
	    grep -oE '_*xtc_[A-Za-z0-9_]+\(' | tr -d '(' | sort -u || true)
	if [ "$n" -eq 0 ]; then
		echo "  [header-api] FAIL: $plat: no declaration found" >&2
		fail=1
	elif [ -n "$missing" ]; then
		echo "  [header-api] FAIL: $plat: declared without XTC_API" \
		    "(not exported from a Windows DLL):" >&2
		printf '%s\n' "$missing" | sed 's/^/        /' >&2
		fail=1
	fi
	ran="$ran $plat($n)"
done
[ "$fail" -eq 0 ] || exit 1
echo "  [header-api] OK: every installed declaration is XTC_API:$ran"
