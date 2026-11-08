#!/bin/sh
# shellcheck disable=SC2086
# test/dist/test_atomic_cxx.sh -- xtc.h compiles as C++, and the atomics
# compile on every backend this host can reach.
#
#  1. A C++ translation unit that includes xtc.h (and so xtc_atomic.h /
#     xtc_compiler.h) and uses every atomic operation compiles and runs
#     with g++ and clang++ (-std=c++11 and -std=c++20), -Wall -Wextra
#     -Werror.  C++ consumers include the same header a C consumer does;
#     no `_Atomic` reaches them through xtc.h.  (xtc_res.h and
#     xtc_lwlock.h still declare `_Atomic` fields until the library is
#     moved onto xtc_atomic.h; they join this check then.)
#  2. The same unit compiles as C with -DXTC_ATOMIC_FORCE_C11, the
#     <stdatomic.h> fallback.
#  3. The MSVC backend (Interlocked* / __iso_volatile_*) compiles, C and
#     C++, for x86_64 and aarch64, using clang with a *-windows-msvc
#     target and -DXTC_ATOMIC_FORCE_MSVC (clang implements the MSVC
#     intrinsics); a second pass with __clang__ undefined takes every
#     cl.exe-only branch.  Syntax and types only: it is not run here; the
#     windows-msvc CI job builds the library (still with
#     /experimental:c11atomics) and runs test/msvc/smoke.c.
#
# A missing compiler is a SKIP for that leg, said by name.  Exit 0 = every
# available leg passed.
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
INC="$XTC_SRC_DIR/src/inc"
work=$(mktemp -d "${TMPDIR:-/tmp}/xtcatomcxx.XXXXXX") || exit 1
trap 'find "$work" -delete 2>/dev/null || true' EXIT INT TERM

cat > "$work/t.cc" <<'EOF'
#include "xtc.h"
#include <stdint.h>

struct node {
	int v;
	struct node *next;
};
typedef int (*fn_t)(int);
static int twice(int x) { return 2 * x; }

static int64_t i64;
static uint8_t u8;
static size_t sz;
static struct node *head;
static fn_t fp;
static XTC_THREAD_LOCAL int tl;
struct padded {
	char c;
	XTC_ALIGNED(XTC_CACHE_LINE) uint64_t hot;
};

int
main(void)
{
	struct node a = { 1, 0 };
	struct node *e = 0, *got;
	int64_t ex = 0;
	int rc = 0;

	xtc_atomic_store_i64(&i64, 5, xtc_mo_release);
	rc |= xtc_atomic_load_i64(&i64, xtc_mo_acquire) != 5;
	rc |= xtc_atomic_fetch_add_i64(&i64, 2, xtc_mo_relaxed) != 5;
	rc |= xtc_atomic_fetch_sub_i64(&i64, 1, xtc_mo_acq_rel) != 7;
	rc |= xtc_atomic_exchange_i64(&i64, 9, xtc_mo_seq_cst) != 6;
	ex = 9;
	rc |= !xtc_atomic_cas_i64(&i64, &ex, 1, xtc_mo_acq_rel,
	    xtc_mo_acquire);
	(void)xtc_atomic_cas_weak_i64(&i64, &ex, 1, xtc_mo_relaxed,
	    xtc_mo_relaxed);
	rc |= xtc_atomic_fetch_or_u8(&u8, 3, xtc_mo_relaxed) != 0;
	rc |= xtc_atomic_fetch_and_u8(&u8, 1, xtc_mo_relaxed) != 3;
	rc |= xtc_atomic_fetch_xor_u8(&u8, 1, xtc_mo_relaxed) != 1;
	xtc_atomic_store_size(&sz, 4, xtc_mo_relaxed);

	xtc_atomic_store_ptr(&head, &a, xtc_mo_release);
	got = (struct node *)xtc_atomic_load_ptr(&head, xtc_mo_acquire);
	rc |= got->v != 1;
	e = &a;
	rc |= !xtc_atomic_cas_ptr(&head, &e, (struct node *)0,
	    xtc_mo_acq_rel, xtc_mo_acquire);
	rc |= (struct node *)xtc_atomic_exchange_ptr(&head, &a,
	    xtc_mo_seq_cst) != 0;
#if !defined(XTC_ATOMIC_FORCE_C11) /* ISO C: no fn pointer via void * */
	xtc_atomic_store_ptr(&fp, twice, xtc_mo_release);
	rc |= ((fn_t)xtc_atomic_load_ptr(&fp, xtc_mo_acquire))(2) != 4;
#else
	fp = twice;
	rc |= fp(2) != 4;
#endif
	xtc_atomic_store_ptr(&head, NULL, xtc_mo_relaxed);
	xtc_atomic_store_ptr(&head, 0, xtc_mo_relaxed);

	xtc_atomic_thread_fence(xtc_mo_seq_cst);
	xtc_atomic_signal_fence(xtc_mo_acq_rel);
	XTC_CPU_RELAX();
	tl = xtc_ctz64(8) + xtc_clz64(1) + xtc_popcount64(7);
	rc |= tl != 3 + 63 + 3;
	rc |= XTC_UNLIKELY(sizeof(struct padded) != 2 * XTC_CACHE_LINE);
	rc |= !XTC_LIKELY(xtc_ctz32(2) == 1 && xtc_clz32(1) == 31 &&
	    xtc_popcount32(3) == 2);
	return rc;
}
EOF
cp "$work/t.cc" "$work/t.c"

ran=""
skipped=""
fail=0
# a multi-flag string, word-split on purpose where it is used (SC2086)
WARN="-Wall -Wextra -Werror"

leg() {	# name, then the compile command; runs the binary if one is made
	_name=$1
	shift
	if ! "$@" > "$work/out" 2>&1; then
		echo "  [atomic-cxx] FAIL: $_name:" >&2
		sed 's/^/        /' "$work/out" >&2
		fail=1
		return
	fi
	if [ -x "$work/a.out" ]; then
		if ! "$work/a.out"; then
			echo "  [atomic-cxx] FAIL: $_name: the program failed" >&2
			fail=1
		fi
		rm -f "$work/a.out"
	fi
	ran="$ran $_name"
}

for cxx in g++ clang++; do
	if ! command -v "$cxx" >/dev/null 2>&1; then
		skipped="$skipped $cxx"
		continue
	fi
	for std in c++11 c++20; do
		leg "$cxx/$std" "$cxx" -std="$std" $WARN -I"$INC" \
		    -o "$work/a.out" "$work/t.cc"
	done
done

for cc in gcc clang; do
	if ! command -v "$cc" >/dev/null 2>&1; then
		skipped="$skipped $cc"
		continue
	fi
	leg "$cc/c11-fallback" "$cc" -std=c11 $WARN -Wpedantic \
	    -DXTC_ATOMIC_FORCE_C11 -I"$INC" -o "$work/a.out" "$work/t.c"
done

# MSVC backend through clang's MSVC-compatible targets.  The nix clang
# wrapper adds -fPIC (hardening), which a Windows target rejects.  The
# probe includes <stddef.h> and <intrin.h> for BOTH targets: a clang
# without its own Windows-target headers (FreeBSD's base clang) accepts
# the target but cannot compile xtc.h for it, which is a SKIP, not a
# FAIL.
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
			src="$work/t.c"
			[ "$lang" = c++ ] && src="$work/t.cc"
			leg "msvc/$tgt/$lang" env NIX_HARDENING_ENABLE='' clang \
			    --target="$tgt" -ffreestanding -fms-extensions \
			    -fms-compatibility -DXTC_ATOMIC_FORCE_MSVC $WARN \
			    -Wno-unused-command-line-argument \
			    -I"$INC" -x "$lang" -c -o "$work/msvc.o" "$src"
			# __clang__ undefined: the cl.exe-only branches of
			# xtc_compiler.h (_BitScan*, _mm_pause/__yield,
			# __declspec(thread)) are compiled too.
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
	echo "  [atomic-cxx] SKIP: no C/C++ compiler found"
	exit 0
fi
echo "  [atomic-cxx] OK:$ran${skipped:+ (SKIP:$skipped)}"
