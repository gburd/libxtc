#!/bin/sh
# dist/xtc_probes.sh -- build the libxtc USDT probe artifacts.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# Both build systems (dist/Makefile.in and meson.build) call this, so the
# probe recipe exists once.  See src/inc/xtc_probes.h for the three probe
# builds and xtc_usdt(3) for the probe set.
#
#   xtc_probes.sh header DTRACE PROBES.D OUT.h
#	`dtrace -h`, then rename the generated XTC_ macros to XTC_DT_ so
#	they cannot collide with a libxtc macro (PostgreSQL renames its
#	generated macros the same way, TRACE_ -> TRACE_POSTGRESQL_).
#
#   xtc_probes.sh object DTRACE PROBES.D OUT.o OBJ...
#	`dtrace -G` over the library objects -- on FreeBSD and illumos it
#	rewrites each probe call site to nops IN PLACE and emits the probe
#	table; the systemtap `dtrace` on Linux emits the is-enabled
#	semaphores -- then `ld -r` the result with an anchor symbol into
#	OUT.o.  Every probe object references the anchor
#	(XTC_PROBES_ANCHOR in xtc_probes.h), so a static link against
#	libxtc.a pulls OUT.o in.  Without the anchor nothing names that
#	archive member, the linker skips it, and the probes silently
#	vanish from every statically linked program.  Re-running over
#	objects an earlier run already rewrote is safe.  Not used on
#	macOS, where the linker builds the probe table itself.
#
# Environment: CC (default cc), LD (default ld), DTRACEFLAGS (e.g. -64).
set -eu

if [ $# -lt 4 ]; then
	echo "usage: $0 header|object DTRACE PROBES.D OUT [OBJ...]" >&2
	exit 2
fi
mode=$1
dtrace=$2
defs=$3
out=$4
shift 4
dir=$(dirname "$out")

case "$mode" in
header)
	# shellcheck disable=SC2086  # DTRACEFLAGS is a word list
	"$dtrace" ${DTRACEFLAGS:-} -h -s "$defs" -o "$dir/xtc_probes_gen.h"
	sed -e 's/\([^A-Za-z0-9_]\)XTC_/\1XTC_DT_/g' -e 's/^XTC_/XTC_DT_/' \
	    "$dir/xtc_probes_gen.h" > "$out"
	rm -f "$dir/xtc_probes_gen.h"
	;;
object)
	[ $# -gt 0 ] || { echo "$0: object mode needs objects" >&2; exit 2; }
	printf '%s\n' \
	    '__attribute__((visibility("hidden"))) const char __xtc_probes_anchor = 0;' \
	    > "$dir/xtc_probes_anchor.c"
	${CC:-cc} -fPIC -c -o "$dir/xtc_probes_anchor.o" "$dir/xtc_probes_anchor.c"
	# shellcheck disable=SC2086  # DTRACEFLAGS is a word list
	"$dtrace" ${DTRACEFLAGS:-} -G -s "$defs" -o "$dir/xtc_probes_dof.o" "$@"
	${LD:-ld} -r -o "$out" "$dir/xtc_probes_dof.o" "$dir/xtc_probes_anchor.o"
	rm -f "$dir/xtc_probes_dof.o" "$dir/xtc_probes_anchor.c" \
	    "$dir/xtc_probes_anchor.o"
	;;
*)
	echo "usage: $0 header|object DTRACE PROBES.D OUT [OBJ...]" >&2
	exit 2
	;;
esac
