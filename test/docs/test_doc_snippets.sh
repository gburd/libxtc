#!/bin/sh
# test/docs/test_doc_snippets.sh -- documentation code is a release gate.
#
# Compiles every docs/_includes/snippets/*.c against the freshly built
# static library and runs each one that has a main().  A snippet marked
# `/* SNIPPET: compile-only */` on its first line is compiled but not
# run (it is an illustrative fragment, e.g. a signature or a partial).
# A snippet with an NN_name.out beside it must print exactly that file
# (after the normalizer below), so an output block a page shows cannot
# drift from what the program prints.
#
# The pages are checked too (docs/**/*.md and README.md), so a program
# cannot be pasted into a page where nothing builds it:
#   - every ```c fence has `<!-- fragment -->` on the line above it (a
#     partial the page does not claim builds) or `<!-- snippet: NN_name.c
#     -->` (the fence must equal that file's "full" region, byte for
#     byte);
#   - `<!-- output: NN_name.out -->` above a fence: the fence must equal
#     that file;
#   - `{% include output.html file="NN_name.out" %}` must name a file
#     that exists.
# Jekyll pages include snippets and outputs (snippet.html, output.html)
# and need no copy.  The two markers serve the pages a forge renders as
# plain Markdown (README.md, docs/getting-started.md), where an include
# would show as raw Liquid.
#
# Env:
#   XTC_SRC_DIR  repo root (default: the dir two levels above this script)
#   XTC_BUILD    build dir holding libxtc.a (default: $XTC_SRC_DIR/build_unix)
#   CC           compiler (default: cc)
set -eu

here=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
SRC=${XTC_SRC_DIR:-$(unset CDPATH; cd -- "$here/../.." && pwd)}
BUILD=${XTC_BUILD:-${XTC_BUILD_DIR:-$SRC/build_unix}}
CC=${CC:-cc}
# The library may have been built with sanitizers or other flags that its
# object files then require at link time (e.g. -fsanitize=address pulls in the
# ASan runtime).  A snippet linked against such a library must be built with
# the same flags or the link fails with undefined __asan_* references.  make
# check passes the build's CFLAGS/LDFLAGS through XTC_SNIPPET_CFLAGS /
# XTC_SNIPPET_LDFLAGS; default to none for a plain build.
SNIP_CFLAGS=${XTC_SNIPPET_CFLAGS:-}
SNIP_LDFLAGS=${XTC_SNIPPET_LDFLAGS:-}
LIB="$BUILD/libxtc.a"
INC="$SRC/src/inc"
SNIP="$SRC/docs/_includes/snippets"

tmpd=$(mktemp -d "${TMPDIR:-/tmp}/xtc-docpage.XXXXXX")
trap 'rm -rf "$tmpd" 2>/dev/null || :' EXIT

# The page checks need no library, so they run first and always.
pages=$( (find "$SRC/docs" -name '*.md' ! -path "$SRC/docs/_site/*" \
    ! -path "$SRC/docs/vendor/*" ! -path "$SRC/docs/api/*" \
    ! -path "$SRC/docs/_includes/*"; \
    echo "$SRC/README.md") | sort)
pagefail=0
for p in $pages; do
	rel=${p#"$SRC"/}
	# One line per finding: "BAD <where> <message>", "SNIP|OUT <line>
	# <name> <file holding the fence body>", or "INC <line> <name> -".
	findings=$(awk -v rel="$rel" -v dir="$tmpd" '
	    function fence(s) { return s ~ /^[ \t]*```/ }
	    FNR == 1 { inb = 0; prev = ""; n = 0 }
	    {
		if (inb) {
			if (fence($0)) {
				inb = 0
				if (body != "") close(body)
				prev = $0
				next
			}
			if (body != "") print $0 > body
			next
		}
		if (fence($0)) {
			inb = 1; body = ""; kind = ""
			if (prev ~ /<!-- snippet: [A-Za-z0-9_]+\.c -->/)
				kind = "SNIP"
			else if (prev ~ /<!-- output: [A-Za-z0-9_]+\.out -->/)
				kind = "OUT"
			if (kind != "") {
				name = prev
				sub(/^.*<!-- [a-z]+: /, "", name)
				sub(/ -->.*$/, "", name)
				n++
				body = dir "/" n
				printf "" > body
				print kind, FNR, name, body
			} else if ($0 ~ /^[ \t]*```[ \t]*[cC][ \t]*$/ &&
			    prev !~ /<!-- fragment -->/) {
				print "BAD", rel ":" FNR ":", "a ```c block that" \
				    " is neither <!-- snippet: NN_name.c --> nor" \
				    " <!-- fragment -->"
			}
			prev = $0
			next
		}
		if ($0 ~ /include output\.html file="/) {
			name = $0
			sub(/^.*file="/, "", name)
			sub(/".*$/, "", name)
			print "INC", FNR, name, "-"
		}
		prev = $0
	    }' "$p")
	while read -r kind line name body; do
		case $kind in
		BAD)
			echo "[docs] FAIL: $line $name $body" >&2
			pagefail=$((pagefail + 1))
			;;
		SNIP)
			if [ ! -f "$SNIP/$name" ]; then
				echo "[docs] FAIL: $rel:$line: no snippet $name" >&2
				pagefail=$((pagefail + 1))
			elif ! sed -n '/\/\* !region full \*\//,/\/\* !endregion full \*\//p' \
			    "$SNIP/$name" | sed '1d;$d' | diff -u - "$body" >"$tmpd/d" 2>&1; then
				echo "[docs] FAIL: $rel:$line: the block differs from" \
				    "$name (- snippet, + page)" >&2
				sed 's/^/    /' "$tmpd/d" >&2
				pagefail=$((pagefail + 1))
			fi
			;;
		OUT)
			if [ ! -f "$SNIP/$name" ]; then
				echo "[docs] FAIL: $rel:$line: no output file $name" >&2
				pagefail=$((pagefail + 1))
			elif ! diff -u "$SNIP/$name" "$body" >"$tmpd/d" 2>&1; then
				echo "[docs] FAIL: $rel:$line: the block differs from" \
				    "$name (- file, + page)" >&2
				sed 's/^/    /' "$tmpd/d" >&2
				pagefail=$((pagefail + 1))
			fi
			;;
		INC)
			if [ ! -f "$SNIP/$name" ]; then
				echo "[docs] FAIL: $rel:$line: no output file $name" >&2
				pagefail=$((pagefail + 1))
			fi
			;;
		esac
	done <<EOF
$findings
EOF
done
# An .out is only a gate if its snippet runs.
for o in "$SNIP"/*.out; do
	[ -e "$o" ] || continue
	c=${o%.out}.c
	if [ ! -f "$c" ] || head -1 "$c" | grep -q 'SNIPPET: compile-only'; then
		echo "[docs] FAIL: $(basename "$o") has no snippet that runs" >&2
		pagefail=$((pagefail + 1))
	fi
done
if [ "$pagefail" -ne 0 ]; then
	echo "[docs] FAIL: $pagefail page problem(s); a program a page shows" \
	    "lives in docs/_includes/snippets/ (see the top of $0)" >&2
	exit 1
fi
echo "[docs] ok: pages ($(echo "$pages" | wc -l | tr -d ' ') checked)"

if [ ! -f "$LIB" ]; then
	echo "[docs] SKIP: no static library at $LIB (build libxtc first)" >&2
	exit 0
fi
# The snippets are programs that run their loops for real.  A sim build's
# xtc_loop_run returns XTC_E_STATE outside a simulation run, so every
# snippet with a loop would fail for a reason that is not the doc's.
if nm "$LIB" 2>/dev/null | grep -q ' T __xtc_io_sim_next_due$'; then
	echo "[docs] FAIL: $LIB is a --with-io-backend=sim build; the snippets" \
	    "need a native backend (point XTC_BUILD at one)" >&2
	exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/xtc-docsnip.XXXXXX")
# Remove the whole scratch tree on exit.  Must be a recursive remove, not
# `rm -f "$tmp"/*; rmdir "$tmp"`: on macOS a `-g` build drops a .dSYM
# DIRECTORY beside each binary, which `rm -f` cannot delete, so the stray
# dirs make the trailing `rmdir` fail -- and under `set -eu` that failing
# final trap command becomes the script's exit status, failing the gate
# AFTER it has already printed success.  `rm -rf` clears it in one step
# and cannot leave a nonzero behind.  $tmp is our own mktemp -d, never
# empty-or-unset here, so the recursive remove is bounded to it.
trap 'rm -rf "$tmp" "$tmpd" 2>/dev/null || :' EXIT

# Link line mirrors the examples: static lib + pthreads + libm + libdl,
# and liburing only if the library was built against it.
LIBS="-lpthread -lm"
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing 2>/dev/null; then
	if nm "$LIB" 2>/dev/null | grep -q io_uring_queue_init; then
		LIBS="$LIBS $(pkg-config --libs liburing)"
	fi
fi
# TLS: a library built with a real TLS backend needs that backend's libs
# (and the configure-time -L path, for a backend in a non-default prefix)
# to link any snippet that calls xtc_tls_*.  Read them from the build's
# own Makefile rather than guess: XTC_TLS_LIBS is exactly what the library
# was configured with.  Before this, a TLS snippet failed to link on every
# TLS backend, so it had to live outside the gate.
if [ -f "$BUILD/Makefile" ] && ! nm "$LIB" 2>/dev/null | grep -q '^tls_none\.o:'; then
	tls_libs=$(sed -n 's/^XTC_TLS_LIBS[[:space:]]*=[[:space:]]*//p' "$BUILD/Makefile" | head -1)
	tls_ldf=$(sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' "$BUILD/Makefile" | head -1)
	[ -n "$tls_libs" ] && LIBS="$LIBS $tls_ldf $tls_libs"
fi
# The platform's socket libraries (illumos/Solaris: -lsocket -lnsl), as
# configure found them for the library; without them every network snippet
# failed to link on OmniOS (send/recv/accept undefined).
if [ -f "$BUILD/Makefile" ]; then
	plat_libs=$(sed -n 's/^XTC_PLATFORM_LIBS[[:space:]]*=[[:space:]]*//p' "$BUILD/Makefile" | head -1)
	[ -n "$plat_libs" ] && LIBS="$LIBS $plat_libs"
fi
# libdl is folded into libc on modern glibc; add it only if present.
printf 'int main(void){return 0;}\n' > "$tmp/dltest.c"
if $CC "$tmp/dltest.c" -ldl -o "$tmp/dltest" >/dev/null 2>&1; then
	LIBS="$LIBS -ldl"
fi
# librt likewise: on glibc timer_create/clock_* are in libc, but on FreeBSD
# they live in librt and the preemption timer (xtc_preempt_arm) needs it --
# without this EVERY snippet failed to link there, so the gate reported
# "10 of 10 broken" on a healthy library.  Probe rather than hardcode, so a
# platform without librt is unaffected.
if $CC "$tmp/dltest.c" -lrt -o "$tmp/dltest" >/dev/null 2>&1; then
	LIBS="$LIBS -lrt"
fi

# Rewrite, in both the expected and the real output, only the values a
# page itself says vary from run to run or host to host; everything else
# must match exactly.  Each rule names the snippet and the value it
# covers.  No snippet with an .out prints a pid, an address, a timing or
# an HLC value today, so there is no rule for one.
normalize() {
	# 06_qsort_yield: the yield count is the number of comparisons the
	# libc's qsort makes, which chapter 2 says varies (17 is glibc's).
	sed -e 's/^sorted correctly through [0-9][0-9]* in-qsort yields:/sorted correctly through N in-qsort yields:/'
}

n=0
fail=0
for f in "$SNIP"/*.c; do
	[ -e "$f" ] || continue
	n=$((n + 1))
	base=$(basename "$f" .c)
	bin="$tmp/$base"
	# $LIBS is a deliberate multi-flag string; word-splitting is intended.
	# shellcheck disable=SC2086
	if ! $CC -std=c11 -D_GNU_SOURCE -Wall -Wextra $SNIP_CFLAGS -I"$INC" \
		-o "$bin" "$f" "$LIB" $SNIP_LDFLAGS $LIBS >"$tmp/$base.log" 2>&1; then
		echo "[docs] FAIL: $base did not compile" >&2
		sed 's/^/    /' "$tmp/$base.log" >&2
		fail=$((fail + 1))
		continue
	fi
	if head -1 "$f" | grep -q 'SNIPPET: compile-only'; then
		echo "[docs] ok (compile-only): $base"
		continue
	fi
	if ! "$bin" >"$tmp/$base.run" 2>"$tmp/$base.err"; then
		echo "[docs] FAIL: $base compiled but exited non-zero" >&2
		sed 's/^/    /' "$tmp/$base.run" "$tmp/$base.err" >&2
		fail=$((fail + 1))
		continue
	fi
	if [ -f "$SNIP/$base.out" ]; then
		normalize <"$SNIP/$base.out" >"$tmp/$base.want"
		normalize <"$tmp/$base.run" >"$tmp/$base.got"
		if ! diff -u "$tmp/$base.want" "$tmp/$base.got" >"$tmp/$base.diff"; then
			echo "[docs] FAIL: $base printed something other than" \
			    "$base.out (- file, + program)" >&2
			sed 's/^/    /' "$tmp/$base.diff" >&2
			fail=$((fail + 1))
			continue
		fi
	fi
	echo "[docs] ok: $base"
done

if [ "$fail" -ne 0 ]; then
	echo "[docs] FAIL: $fail of $n documentation snippet(s) broken" >&2
	exit 1
fi
echo "[docs] OK: all $n documentation snippet(s) compile and run"
echo "[docs] OK: every .out matches its snippet's output"
