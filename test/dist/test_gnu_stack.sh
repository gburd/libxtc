#!/bin/sh
# test/dist/test_gnu_stack.sh -- libxtc must not ask for an executable
# stack.
#
# Why: every src/os/asm/*.S is assembled on every target, and on a
# non-matching arch the object is empty.  An ELF object with no
# .note.GNU-stack section makes ld assume it needs an executable stack,
# and ld ORs that into the whole output: libxtc.so got GNU_STACK RWE
# ("missing .note.GNU-stack section implies executable stack"), and a
# process that loads it runs with an rwx [stack] (glibc makes every
# thread stack executable for it).  The archive alone hid it: a static
# link never pulls an archive member that defines nothing.
#
# Checks (ELF targets only):
#   1. the shared library's GNU_STACK segment is RW, not RWE.  It is the
#      build's libxtc.so when configure ran with --enable-shared, else a
#      scratch DSO linked from every member of libxtc.a (--whole-archive:
#      the same object set the shared-library rule links);
#   2. a consumer linked against that DSO, and one linked statically
#      against libxtc.a, link without the "executable stack" warning and
#      run with an rw-p [stack].
#
# Env: XTC_BUILD_DIR (the build tree; default .), CC.
set -u
B=${XTC_BUILD_DIR:-.}
CC=${CC:-cc}
A=$B/libxtc.a
if [ ! -f "$A" ]; then
	echo "  [gnu_stack] SKIP: no $A"
	exit 0
fi
if ! command -v readelf >/dev/null 2>&1; then
	echo "  [gnu_stack] SKIP: no readelf"
	exit 0
fi
if ! $CC -dM -E - </dev/null 2>/dev/null | grep -q '__ELF__'; then
	echo "  [gnu_stack] SKIP: not an ELF target"
	exit 0
fi
LIBS=$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' "$B/Makefile" 2>/dev/null)
LDF=$(sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' "$B/Makefile" 2>/dev/null)
W=$(mktemp -d "${TMPDIR:-/tmp}/xtc_gnu_stack.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT INT TERM
fail=0

# Prints GNU_STACK's flags field (RW / RWE), or nothing if absent.
stack_flags() {
	readelf -lW "$1" 2>/dev/null | awk '$1 == "GNU_STACK" { print $(NF-1) }'
}

# A link log that names an executable stack is a failure.
check_log() {
	if grep -i 'executable stack' "$1" >/dev/null 2>&1; then
		echo "  [gnu_stack] FAIL: $2 link warns:" >&2
		grep -i 'executable stack' "$1" | sed 's/^/            /' >&2
		fail=1
	fi
}

# check_maps LABEL CMD...: run the consumer, check its [stack] mapping.
check_maps() {
	_lbl=$1
	shift
	[ -r /proc/self/maps ] || return 0
	perm=$("$@" 2>/dev/null)
	case "$perm" in
	rw-p) ;;
	*)	echo "  [gnu_stack] FAIL: $_lbl: [stack] is '$perm', want rw-p" >&2
		fail=1 ;;
	esac
}

SO=$B/libxtc.so
if [ -f "$SO" ]; then
	what="libxtc.so"
else
	SO=$W/libxtc_scratch.so
	what="scratch DSO of every libxtc.a member"
	# shellcheck disable=SC2086
	if ! $CC -shared -o "$SO" -Wl,--whole-archive "$A" \
	    -Wl,--no-whole-archive $LDF $LIBS >"$W/so.log" 2>&1; then
		echo "  [gnu_stack] SKIP: cannot link a scratch DSO here:"
		sed 's/^/            /' "$W/so.log" | head -5
		exit 0
	fi
	check_log "$W/so.log" "the scratch DSO"
fi
fl=$(stack_flags "$SO")
if [ "$fl" != "RW" ]; then
	echo "  [gnu_stack] FAIL: $what: GNU_STACK is '${fl:-absent}', want RW" >&2
	readelf -lW "$SO" | grep GNU_STACK | sed 's/^/            /' >&2
	fail=1
fi

cat >"$W/c.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "xtc.h"
int
main(void)
{
	char l[512], p[8] = "";
	FILE *f = fopen("/proc/self/maps", "r");
	if (xtc_version_string() == NULL || f == NULL)
		return 1;
	while (fgets(l, sizeof l, f) != NULL)
		if (strstr(l, "[stack]") != NULL)
			(void)sscanf(l, "%*s %7s", p);
	fclose(f);
	printf("%s\n", p);
	return 0;
}
EOF
INC="-I${XTC_SRC_DIR:-$B/..}/src/inc -I$B"
# shellcheck disable=SC2086
if $CC $INC -o "$W/c_dyn" "$W/c.c" -Wl,--no-as-needed "$SO" $LDF $LIBS \
    >"$W/dyn.log" 2>&1; then
	check_log "$W/dyn.log" "a consumer of $what"
	check_maps "a consumer of $what" env LD_LIBRARY_PATH="$(dirname "$SO")" "$W/c_dyn"
else
	echo "  [gnu_stack] FAIL: consumer of $what did not link" >&2
	sed 's/^/            /' "$W/dyn.log" | head -5 >&2
	fail=1
fi
# shellcheck disable=SC2086
if $CC $INC -o "$W/c_st" "$W/c.c" "$A" $LDF $LIBS >"$W/st.log" 2>&1; then
	check_log "$W/st.log" "a static consumer"
	check_maps "a static consumer" "$W/c_st"
else
	echo "  [gnu_stack] FAIL: static consumer did not link" >&2
	sed 's/^/            /' "$W/st.log" | head -5 >&2
	fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "  [gnu_stack] OK: $what GNU_STACK RW; dynamic and static consumers" \
    "link clean and run with an rw-p [stack]"
exit 0
