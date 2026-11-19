#!/bin/sh
# shellcheck disable=SC2086
# test/dist/test_header_cxx.sh -- libxtc's headers coexist with C++.
#
# libxtc is a C11 library with no C++ API.  What a C++ program gets is
# coexistence: it can include the installed headers and link the library.
# This gate holds exactly that, and nothing more:
#
#  (a) Every installed header (LIB_HDRS_PUBLIC in dist/Makefile.in)
#      compiles ALONE as a C++ translation unit with g++ and clang++,
#      -std=c++11 and -std=c++20, -Wall -Wextra -Werror.
#  (b) test/msvc/cxx_coexist.cpp, a C++ program that includes every
#      installed header and calls C functions from three of them, links
#      against libxtc.a (with a second unit that takes the address of
#      every function the installed headers declare and libxtc.a
#      defines) and runs.  A declaration without extern "C" fails the
#      link: the C++ compiler mangles the name and libxtc.a has no such
#      symbol.  The program's include list must be LIB_HDRS_PUBLIC.
#  (c) xtc.h compiles through clang's *-windows-msvc targets for x86_64
#      and aarch64, as C (where it carries the MSVC branches of
#      xtc_atomic.h and xtc_compiler.h, which are C only) and as C++; a
#      second pass with __clang__ undefined takes every cl.exe-only
#      branch.  Syntax only, nothing linked or run (a cross clang has no
#      Windows SDK headers).
#      dist/build_msvc.bat (the windows-msvc CI job) compiles every
#      installed header with cl /TP and links a C++ program against
#      xtc.lib on a Windows host.
#
# A missing compiler is a SKIP for that leg, said by name.  Exit 0 = every
# available leg passed.
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"
INC="$XTC_SRC_DIR/src/inc"
LIB="$XTC_BUILD_DIR/libxtc.a"
work=$(mktemp -d "${TMPDIR:-/tmp}/xtchdrcxx.XXXXXX") || exit 1
trap 'find "$work" -delete 2>/dev/null || true' EXIT INT TERM
LC_ALL=C
export LC_ALL

# The installed header set: the one list both build systems install.
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
	echo "  [header-cxx] FAIL: no LIB_HDRS_PUBLIC in dist/Makefile.in" >&2
	exit 1
fi

# The link line the library was configured with (liburing, TLS, ...).
# make check passes the build's instrumentation link words (-fsanitize=,
# --coverage and the compiler's coverage runtime) as XTC_INSTR_LDFLAGS:
# an instrumented library needs that runtime on the C++ link too, after
# libxtc.a, and clang++ needs it named when gcc built the library.
LIBS="-lpthread -lm"
if [ -f "$XTC_BUILD_DIR/Makefile" ]; then
	LIBS="$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' \
	    "$XTC_BUILD_DIR/Makefile" | head -1) $LIBS"
fi
INSTR_LDFLAGS=${XTC_INSTR_LDFLAGS:-}

# The functions to reference in (b): an xtc_* name followed by "(" in an
# installed header with comments stripped, that is also a text symbol in
# libxtc.a (macros, typedefs and other platforms' functions drop out).
: > "$work/funcs"
if [ -f "$LIB" ] && command -v nm >/dev/null 2>&1; then
	for h in $hdrs; do
		awk '
		BEGIN { inblk = 0 }
		{
			line = $0; out = ""; i = 1; n = length(line)
			while (i <= n) {
				c2 = substr(line, i, 2)
				if (inblk) {
					if (c2 == "*/") { inblk = 0; i += 2 } else { i++ }
				} else if (c2 == "/*") { inblk = 1; i += 2 }
				else if (c2 == "//") { break }
				else { out = out substr(line, i, 1); i++ }
			}
			print out
		}' "$INC/$h"
	done | grep -oE '(^|[^A-Za-z0-9_])xtc_[A-Za-z0-9_]+[[:space:]]*[(]' |
	    sed -E 's/^[^x]*//; s/[[:space:]]*[(]$//' | sort -u > "$work/declared"
	nm -g "$LIB" 2>/dev/null |
	    awk '$2 == "T" { n = $3; sub(/^_/, "", n); if (n ~ /^xtc_/) print n }' |
	    sort -u > "$work/defined"
	comm -12 "$work/declared" "$work/defined" > "$work/funcs"
fi

# cxx_coexist.cpp is also what dist/build_msvc.bat builds with cl /TP, so
# its include list is checked against the installed set here.
PROG="$XTC_SRC_DIR/test/msvc/cxx_coexist.cpp"
sed -n 's/^#include <\(xtc[_A-Za-z0-9]*\.h\)>$/\1/p' "$PROG" | sort > "$work/prog.h"
echo "$hdrs" | sort > "$work/inst.h"
if ! cmp -s "$work/prog.h" "$work/inst.h"; then
	echo "  [header-cxx] FAIL: test/msvc/cxx_coexist.cpp must include" \
	    "exactly LIB_HDRS_PUBLIC (< program, > installed):" >&2
	diff "$work/prog.h" "$work/inst.h" | grep '^[<>]' | sed 's/^/        /' >&2
	exit 1
fi

for h in $hdrs; do
	echo "#include \"$h\""
done > "$work/refs.cc"
{
	echo 'typedef void (*xtc_cxx_fn)(void);'
	echo 'extern const xtc_cxx_fn xtc_cxx_refs[];'
	echo 'const xtc_cxx_fn xtc_cxx_refs[] = {'
	sed 's/.*/	(xtc_cxx_fn)\&&,/' "$work/funcs"
	echo '	0'
	echo '};'
} >> "$work/refs.cc"

ran=""
skipped=""
fail=0
# a multi-flag string, word-split on purpose where it is used (SC2086)
WARN="-Wall -Wextra -Werror"

leg() {	# name, then the compile command; runs a.out if one is made
	_name=$1
	shift
	if ! "$@" > "$work/out" 2>&1; then
		echo "  [header-cxx] FAIL: $_name:" >&2
		sed 's/^/        /' "$work/out" | head -40 >&2
		fail=1
		return
	fi
	if [ -x "$work/a.out" ]; then
		if ! "$work/a.out" > "$work/run.out" 2>&1; then
			sed 's/^/        /' "$work/run.out" >&2
			echo "  [header-cxx] FAIL: $_name: the program failed" >&2
			fail=1
		fi
		rm -f "$work/a.out"
	fi
	ran="$ran $_name"
}

alone() {	# compiler, std: every installed header by itself
	_bad=""
	for h in $hdrs; do
		echo "#include \"$h\"" > "$work/one.cc"
		if ! "$1" -std="$2" $WARN -I"$INC" -c -o "$work/one.o" \
		    "$work/one.cc" > "$work/one.err" 2>&1; then
			_bad="$_bad $h"
			sed 's/^/        /' "$work/one.err" | head -5 >&2
		fi
	done
	if [ -n "$_bad" ]; then
		echo "  [header-cxx] FAIL: $1/$2: does not compile alone:$_bad" >&2
		fail=1
		return
	fi
	ran="$ran $1/$2"
}

nfuncs=$(wc -l < "$work/funcs" | tr -d ' ')
for cxx in g++ clang++; do
	if ! command -v "$cxx" >/dev/null 2>&1; then
		skipped="$skipped $cxx"
		continue
	fi
	for std in c++11 c++20; do
		alone "$cxx" "$std"
	done
	if [ ! -f "$LIB" ]; then
		skipped="$skipped $cxx/link(no-libxtc.a)"
		continue
	fi
	leg "$cxx/link($nfuncs-fns)" "$cxx" -std=c++11 $WARN \
	    -Wno-deprecated-declarations -I"$INC" -I"$XTC_BUILD_DIR" \
	    -o "$work/a.out" "$PROG" "$work/refs.cc" "$LIB" \
	    $INSTR_LDFLAGS $LIBS
done

# (c) The MSVC targets.  The nix clang wrapper adds -fPIC (hardening),
# which a Windows target rejects.  The probe includes <stddef.h> and
# <intrin.h> for BOTH targets: a clang without its own Windows-target
# headers (FreeBSD's base clang) accepts the target but cannot compile
# xtc.h for it, which is a SKIP, not a FAIL.
echo '#include "xtc.h"' > "$work/m.c"
cp "$work/m.c" "$work/m.cc"
if command -v clang >/dev/null 2>&1 &&
    printf '#include <stddef.h>\n#include <intrin.h>\nint x;\n' | \
    NIX_HARDENING_ENABLE='' clang --target=x86_64-pc-windows-msvc \
    -ffreestanding -fms-extensions -fms-compatibility -x c -c \
    -o "$work/probe.o" - >/dev/null 2>&1 &&
    printf '#include <stddef.h>\n#include <intrin.h>\nint x;\n' | \
    NIX_HARDENING_ENABLE='' clang --target=aarch64-pc-windows-msvc \
    -ffreestanding -fms-extensions -fms-compatibility -x c -c \
    -o "$work/probe.o" - >/dev/null 2>&1; then
	for tgt in x86_64-pc-windows-msvc aarch64-pc-windows-msvc; do
		for lang in c c++; do
			src="$work/m.c"
			[ "$lang" = c++ ] && src="$work/m.cc"
			leg "msvc/$tgt/$lang" env NIX_HARDENING_ENABLE='' clang \
			    --target="$tgt" -ffreestanding -fms-extensions \
			    -fms-compatibility -DXTC_ATOMIC_FORCE_MSVC $WARN \
			    -Wno-unused-command-line-argument \
			    -I"$INC" -x "$lang" -c -o "$work/msvc.o" "$src"
			leg "msvc-cl/$tgt/$lang" env NIX_HARDENING_ENABLE='' clang \
			    --target="$tgt" -U__clang__ \
			    -Wno-builtin-macro-redefined -ffreestanding \
			    -fms-extensions -fms-compatibility $WARN \
			    -Wno-unused-command-line-argument \
			    -I"$INC" -x "$lang" -c -o "$work/msvc.o" "$src"
		done
	done
else
	skipped="$skipped msvc-target"
fi

[ "$fail" -eq 0 ] || exit 1
if [ -z "$ran" ]; then
	echo "  [header-cxx] SKIP: no C++ compiler found"
	exit 0
fi
echo "  [header-cxx] OK:$ran${skipped:+ (SKIP:$skipped)}"
