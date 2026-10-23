#!/bin/sh
# test/dist/test_probes.sh -- the USDT probe set (xtc_usdt(3)).
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
#  [P1] src/xtc_probes.d, src/inc/xtc_probes.h and man/man3/xtc_usdt.3
#       declare the SAME probes: each `probe a__b(...)` has XTC_PROBE_A_B
#       in all three header branches with the same arity, the <sys/sdt.h>
#       branch emits it under the same name, and the man page documents it.
#  [P2] probes off (the default): no probe residue -- no xtc_probes.o, no
#       .note.stapsdt section, no dtrace symbol in libxtc.a.
#  [P3] dtrace build: `dtrace -l -P 'xtc$target' -c probe_workload` lists
#       every probe in a statically linked program.  Needs root.
#  [P4] dtrace build: a D script over probe_workload counts exactly the
#       spawns, exits, messages, contended waits and parks the workload
#       performs by construction.  Needs root.
#  [P5] sdt build: readelf finds the stapsdt notes perf and bpftrace use.
#  [P6] dtrace build: every tools/dtrace/*.d script compiles and runs
#       against the workload.  Needs root.
#
# Env (from make check): XTC_SRC_DIR, XTC_BUILD_DIR, CC, XTC_SNIPPET_CFLAGS/LDFLAGS.
set -eu

: "${XTC_SRC_DIR:?}"
: "${XTC_BUILD_DIR:?}"
CC=${CC:-cc}
D="$XTC_SRC_DIR/src/xtc_probes.d"
H="$XTC_SRC_DIR/src/inc/xtc_probes.h"
MAN="$XTC_SRC_DIR/man/man3/xtc_usdt.3"
B="$XTC_BUILD_DIR"

fail() { echo "  [$1] FAIL: $2" >&2; exit 1; }

# ---- P1 -----------------------------------------------------------------
# "name arity" per probe in the provider block (a probe may span lines).
pairs=$(awk '
	/^provider/ { inp = 1; next }
	inp && /^};/ { inp = 0 }
	inp { buf = buf " " $0 }
	END {
		n = split(buf, st, ";")
		for (i = 1; i <= n; i++) {
			if (st[i] !~ /probe[ \t]+[a-z0-9_]+\(/) continue
			s = st[i]; sub(/.*probe[ \t]+/, "", s)
			name = s; sub(/\(.*/, "", name)
			a = s; sub(/^[^(]*\(/, "", a); sub(/\).*/, "", a)
			print name, split(a, x, ",")
		}
	}' "$D")
[ -n "$pairs" ] || fail P1 "no probes parsed from $D"
n=0
while read -r p arity; do
	n=$((n + 1))
	mac=XTC_PROBE_$(printf '%s' "$p" | sed 's/__/_/g' | tr '[:lower:]' '[:upper:]')
	defs=$(grep -c "^#define $mac(" "$H" || true)
	[ "$defs" -eq 3 ] ||
	    fail P1 "$mac defined $defs times in xtc_probes.h (want 3: dtrace, sdt, off)"
	for got in $(grep "^#define $mac(" "$H" |
	    sed 's/^#define [A-Z_]*(\([a-z, ]*\)).*/\1/' | awk -F, '{print NF}'); do
		[ "$got" -eq "$arity" ] ||
		    fail P1 "$mac takes $got args in xtc_probes.h, $arity in xtc_probes.d"
	done
	grep -q "DTRACE_PROBE$arity(xtc, $p," "$H" ||
	    fail P1 "the <sys/sdt.h> branch does not emit $p with $arity args"
	dname=$(printf '%s' "$p" | sed 's/__/-/g')
	grep -q "^\.It Sy $dname" "$MAN" ||
	    fail P1 "probe $dname is not documented in man/man3/xtc_usdt.3"
done <<EOF
$pairs
EOF
echo "  [P1] OK: $n probes; xtc_probes.d, xtc_probes.h and xtc_usdt(3) agree"

if [ ! -f "$B/Makefile" ] || [ ! -f "$B/libxtc.a" ]; then
	echo "  [P2] SKIP: no configured build at $B"
	exit 0
fi
mode=$(sed -n 's/^XTC_PROBES_MODE[[:space:]]*=[[:space:]]*//p' "$B/Makefile" | head -1)
mode=${mode:-no}
libs=$(sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' "$B/Makefile" | head -1)

work=$(mktemp -d "${TMPDIR:-/tmp}/xtc-probes.XXXXXX")
trap 'find "$work" -mindepth 1 -delete 2>/dev/null; rmdir "$work" 2>/dev/null || :' EXIT
# The build's CFLAGS/LDFLAGS (make check passes them as XTC_SNIPPET_*, as
# for the doc snippets): a sanitizer libxtc.a needs its runtime at link.
# shellcheck disable=SC2086  # $libs and the flag lists are word lists
$CC -std=c11 -D_DEFAULT_SOURCE ${XTC_SNIPPET_CFLAGS:-} -I"$B" \
    -I"$XTC_SRC_DIR/src/inc" \
    -o "$work/probe_workload" "$XTC_SRC_DIR/test/dist/probe_workload.c" \
    "$B/libxtc.a" ${XTC_SNIPPET_LDFLAGS:-} -pthread $libs || \
    fail P2 "probe_workload did not link"
"$work/probe_workload" || fail P2 "probe_workload failed without a tracer"

# ---- P2 -----------------------------------------------------------------
if [ "$mode" = no ]; then
	[ ! -f "$B/xtc_probes.o" ] || fail P2 "xtc_probes.o exists with probes off"
	if command -v readelf >/dev/null 2>&1 &&
	    readelf -S "$work/probe_workload" 2>/dev/null | grep -q stapsdt; then
		fail P2 "probes-off program carries a .note.stapsdt section"
	fi
	if nm "$B/libxtc.a" 2>/dev/null | grep -q -e '__dtrace_xtc' -e '\$dtrace'; then
		fail P2 "probes-off libxtc.a carries dtrace symbols"
	fi
	echo "  [P2] OK: probes off -- no probe object, notes or symbols"
	echo "  [P3] SKIP: probes off (configure --enable-dtrace to test them)"
	exit 0
fi
echo "  [P2] SKIP: probes are on (--enable-dtrace=$mode)"

# ---- P5 -----------------------------------------------------------------
if [ "$mode" = sdt ]; then
	if ! command -v readelf >/dev/null 2>&1; then
		echo "  [P5] SKIP: no readelf"
		exit 0
	fi
	notes=$(readelf -n "$work/probe_workload")
	for p in proc__spawn proc__start proc__exit msg__send msg__recv \
	    fiber__park fiber__resume lock__wait__start lock__wait__done; do
		printf '%s\n' "$notes" | grep -q "Name: $p\$" ||
		    fail P5 "no stapsdt note for $p"
	done
	echo "  [P5] OK: stapsdt notes present for the workload's probes"
	echo "  [P3] SKIP: sdt build has no dtrace -l; P5 checked the notes"
	exit 0
fi

# ---- P3 / P4 ------------------------------------------------------------
DTRACE=$(sed -n 's/^DTRACE[[:space:]]*=[[:space:]]*//p' "$B/Makefile" | head -1)
if [ "$(id -u)" -ne 0 ]; then
	if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
		DTRACE="sudo -n $DTRACE"
	else
		echo "  [P3] SKIP: dtrace -l on a process needs root (or passwordless sudo)"
		echo "  [P4] SKIP: needs root"
		exit 0
	fi
fi

# shellcheck disable=SC2086  # $DTRACE may be "sudo -n /usr/sbin/dtrace"
listed=$($DTRACE -l -P 'xtc$target' -c "$work/probe_workload" 2>/dev/null |
    awk 'NR > 1 { print $NF }' | sort -u)
missing=
while read -r p arity; do
	dname=$(printf '%s' "$p" | sed 's/__/-/g')
	printf '%s\n' "$listed" | grep -qx "$dname" || missing="$missing $dname"
done <<EOF
$pairs
EOF
[ -z "$missing" ] || fail P3 "dtrace -l does not list:$missing"
echo "  [P3] OK: dtrace -l lists all $n probes in a statically linked program"

cat > "$work/count.d" <<'EOF'
xtc$target:::proc-spawn  { @c["spawn"] = count(); }
xtc$target:::proc-start  { @c["start"] = count(); }
xtc$target:::proc-exit   { @c["exit"] = count(); }
xtc$target:::msg-send    { @c["send"] = count(); }
xtc$target:::msg-recv    { @c["recv"] = count(); }
xtc$target:::lock-wait-start /arg1 == 1/ { @c["lockwait"] = count(); }
xtc$target:::lock-wait-done  /arg1 == 1/ { @c["lockdone"] = count(); }
xtc$target:::fiber-park /arg1 == 6/ { @c["park_sync"] = count(); }
xtc$target:::fiber-park /arg1 == 2/ { @c["park_sleep"] = count(); }
END { printa("%s %@d\n", @c); }
EOF
# shellcheck disable=SC2086
out=$($DTRACE -q -s "$work/count.d" -c "$work/probe_workload" 2>&1) ||
    fail P4 "dtrace over probe_workload failed: $out"
# probe_workload.c: ROUNDS=50, CONTEND=5.
for want in "spawn 2" "start 2" "exit 2" "send 110" "recv 110" \
    "lockwait 5" "lockdone 5" "park_sync 5" "park_sleep 5"; do
	printf '%s\n' "$out" | grep -qx "$want" ||
	    fail P4 "expected '$want'; got: $(printf '%s' "$out" | tr '\n' ';')"
done
echo "  [P4] OK: exact probe counts for the fixed workload"

# ---- P6: every shipped D script compiles and runs ----------------------
nd=0
for s in "$XTC_SRC_DIR"/tools/dtrace/*.d; do
	[ -f "$s" ] || continue
	nd=$((nd + 1))
	# shellcheck disable=SC2086
	if ! $DTRACE -q -s "$s" -c "$work/probe_workload" > "$work/script.out" 2>&1; then
		fail P6 "$(basename "$s") failed: $(tail -3 "$work/script.out" | tr '\n' ' ')"
	fi
done
echo "  [P6] OK: all $nd tools/dtrace scripts run against the workload"
