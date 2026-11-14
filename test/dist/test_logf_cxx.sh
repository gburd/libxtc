#!/bin/sh
# shellcheck disable=SC2086
# test/dist/test_logf_cxx.sh -- xtc_logf from C++ and on the MSVC targets.
#
# xtc_logf is _Generic in C and overloads of xtc_logarg in C++.  This
# checks that one call compiles and prints the same record either way:
#
#  1. A C++ program that calls xtc_logf with an integer of every width, a
#     bool, a double, a char *, NULL, a pointer, an xtc_pid_t and an enum,
#     with zero and with 16 arguments, builds with g++ and clang++
#     (-std=c++11 and -std=c++20, -Wall -Wextra -Wpedantic -Werror)
#     against libxtc.a, runs, and the drained records are compared with
#     the expected text.  The same file compiles and runs as C11 (gcc,
#     clang), so the two paths are held to one output.
#  2. A 17th argument is a compile error in C and in C++.
#  3. xtc_log.h and xtc_logf calls compile for x86_64 and aarch64
#     *-windows-msvc with clang (C with -std=c11, and C++), the way
#     cl.exe /std:c11 sees _Generic.  Syntax only, xtc_log.h alone (a
#     cross clang has no libc or Windows SDK headers); nothing is linked
#     or run there.
#
# A missing compiler is a SKIP for that leg, said by name.  Exit 0 = every
# available leg passed.
set -eu
: "${XTC_SRC_DIR:?XTC_SRC_DIR must be set}"
: "${XTC_BUILD_DIR:?XTC_BUILD_DIR must be set}"
INC="$XTC_SRC_DIR/src/inc"
LIB="$XTC_BUILD_DIR/libxtc.a"
if [ ! -f "$LIB" ]; then
	echo "  [logf-cxx] SKIP: no $LIB"
	exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/xtclogfcxx.XXXXXX") || exit 1
trap 'find "$work" -delete 2>/dev/null || true' EXIT INT TERM

# The link line the library was configured with (liburing, TLS, ...).
LIBS="-lpthread -lm"
if [ -f "$XTC_BUILD_DIR/Makefile" ]; then
	LIBS="$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' \
	    "$XTC_BUILD_DIR/Makefile" | head -1) $LIBS"
fi

cat > "$work/t.c" <<'EOF'
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xtc_proc.h"
#include "xtc_log.h"

static char got[2048];
static size_t ngot;

static int
sink(void *user, xtc_log_level_t lvl, const char *buf, size_t len)
{
	const char *body = (const char *)memchr(buf, ']', len) + 8;

	(void)user;
	(void)lvl;
	len -= (size_t)(body - buf);
	if (ngot + len < sizeof got) {
		memcpy(got + ngot, body, len);
		ngot += len;
	}
	return 0;
}

enum color { RED, GREEN };

int
main(void)
{
	xtc_log_opts_t opts;
	xtc_log_t *log;
	xtc_pid_t pid = { 1, 2, 3 };
	int8_t i8 = INT8_MIN;
	uint16_t u16 = UINT16_MAX;
	int64_t i64 = INT64_MIN;
	uint64_t u64 = UINT64_MAX;
	const char *s = "str";
	char *ns = NULL;
	enum color c = GREEN;
	int one = 1;

	/* Not XTC_LOG_OPTS_DEFAULT: designated initializers are C++20. */
	memset(&opts, 0, sizeof opts);
	opts.size = sizeof opts;
	opts.floor = XTC_LOG_INFO;
	opts.sink = sink;
	opts.sink_fd = -1;
	opts.record_max = 512;
	if (xtc_log_create(&opts, &log) != XTC_OK)
		return 1;
	xtc_log_set_default(log);
	xtc_logf(XTC_LOG_INFO, "plain");
	xtc_logf(XTC_LOG_INFO, "{} {} {} {}", i8, u16, i64, u64);
	xtc_logf(XTC_LOG_INFO, "{} {} {} {:.2} {} {}", one == 1, 'x', c, 0.125,
	    s, ns);
	xtc_logf(XTC_LOG_INFO, "{} {:08x} {:p}", pid, 0xbeefu, (void *)0);
	xtc_logf_to(log, XTC_LOG_INFO, "{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}", 1, 2,
	    3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6);
	xtc_logf(XTC_LOG_INFO, "{} {}", 1);
	(void)xtc_log_drain(log);
	xtc_log_set_default(NULL);
	xtc_log_destroy(log);
	got[ngot] = '\0';
	fputs(got, stdout);
	return 0;
}
EOF
cp "$work/t.c" "$work/t.cc"

# 'x' is an int in C and a char in C++: the same number either way.
cat > "$work/want" <<'EOF'
plain
-128 65535 -9223372036854775808 18446744073709551615
1 120 1 0.12 str (null)
<1.2.3> 0000beef 0x0
1234567890123456
[xtc_logf: 1 arg, 2 {}] 1 {}
EOF

# The MSVC-target unit: xtc_log.h alone (no libc or Windows SDK headers,
# which a cross clang may not have), every argument kind, 0 and 16 args.
cat > "$work/m.c" <<'EOF'
#include "xtc_log.h"
enum color { RED, GREEN };
void
f(xtc_log_t *log, long long ll, unsigned short us, double d, const char *s,
    char *ns, const int *ip, enum color c)
{
	xtc_logf(XTC_LOG_INFO, "plain");
	xtc_logf(XTC_LOG_INFO, "{} {} {} {} {} {} {} {}", ll, us, d, s, ns, ip,
	    c, 'x');
	xtc_logf_to(log, XTC_LOG_INFO, "{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}", 1, 2,
	    3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6);
}
EOF
cp "$work/m.c" "$work/m.cc"

cat > "$work/e17.c" <<'EOF'
#include "xtc_log.h"
void
f(void)
{
	xtc_logf(XTC_LOG_INFO, "x", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
	    14, 15, 16, 17);
}
EOF
cp "$work/e17.c" "$work/e17.cc"

ran=""
skipped=""
fail=0
WARN="-Wall -Wextra -Wpedantic -Werror"

leg() {	# name, then the compile command; runs and checks a.out if made
	_name=$1
	shift
	if ! "$@" > "$work/out" 2>&1; then
		echo "  [logf-cxx] FAIL: $_name:" >&2
		sed 's/^/        /' "$work/out" >&2
		fail=1
		return
	fi
	if [ -x "$work/a.out" ]; then
		if ! "$work/a.out" > "$work/got" 2>&1 ||
		    ! cmp -s "$work/got" "$work/want"; then
			echo "  [logf-cxx] FAIL: $_name: wrong records:" >&2
			diff "$work/want" "$work/got" | sed 's/^/        /' >&2
			fail=1
		fi
		rm -f "$work/a.out"
	fi
	ran="$ran $_name"
}

too_many() {	# name, then the compile command, which must fail
	_name=$1
	shift
	if "$@" > "$work/out" 2>&1; then
		echo "  [logf-cxx] FAIL: $_name: a 17th argument compiled" >&2
		fail=1
		return
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
		    -I"$XTC_BUILD_DIR" -o "$work/a.out" "$work/t.cc" "$LIB" $LIBS
	done
	too_many "$cxx/17-args" "$cxx" -std=c++11 -I"$INC" -c \
	    -o "$work/e.o" "$work/e17.cc"
done
for cc in gcc clang; do
	if ! command -v "$cc" >/dev/null 2>&1; then
		skipped="$skipped $cc"
		continue
	fi
	leg "$cc/c11" "$cc" -std=c11 -D_DEFAULT_SOURCE $WARN -I"$INC" \
	    -I"$XTC_BUILD_DIR" -o "$work/a.out" "$work/t.c" "$LIB" $LIBS
	too_many "$cc/17-args" "$cc" -std=c11 -I"$INC" -c -o "$work/e.o" \
	    "$work/e17.c"
done

# The MSVC targets, as test_atomic_cxx.sh probes them: a clang without
# Windows-target headers is a SKIP.  The nix clang wrapper adds -fPIC,
# which a Windows target rejects.
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
		leg "msvc/$tgt/c" env NIX_HARDENING_ENABLE='' clang \
		    --target="$tgt" -std=c11 -ffreestanding -fms-extensions \
		    -fms-compatibility -Wall -Wextra -Werror \
		    -Wno-unused-command-line-argument -I"$INC" -x c -c \
		    -o "$work/msvc.o" "$work/m.c"
		leg "msvc/$tgt/c++" env NIX_HARDENING_ENABLE='' clang \
		    --target="$tgt" -std=c++14 -ffreestanding -fms-extensions \
		    -fms-compatibility -Wall -Wextra -Werror \
		    -Wno-unused-command-line-argument -I"$INC" -x c++ -c \
		    -o "$work/msvc.o" "$work/m.cc"
	done
else
	skipped="$skipped msvc-target"
fi

[ "$fail" -eq 0 ] || exit 1
if [ -z "$ran" ]; then
	echo "  [logf-cxx] SKIP: no C/C++ compiler found"
	exit 0
fi
echo "  [logf-cxx] OK:$ran${skipped:+ (SKIP:$skipped)}"
