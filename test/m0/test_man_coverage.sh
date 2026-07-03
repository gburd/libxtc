#!/bin/sh
# test/m0/test_man_coverage.sh -- D4
# Every PUBLIC function across the installed public header set must be
# documented (mentioned as an .Nm alias or .Fn entry) in some man3 page.
#
# This scans the FULL public surface, not just the umbrella xtc.h, so a
# subsystem that has a page but omits some of its entry points is
# caught.  Man pages are per-subsystem (named after their first
# function) with .Nm aliases for the rest, so the contract is
# "mentioned in some page", not "has its own <name>.3 file".
set -eu
: "${XTC_SRC_DIR:?}"

INC="$XTC_SRC_DIR/src/inc"
MANDIR="$XTC_SRC_DIR/man/man3"

# The installed public header set, as enumerated by LIB_HDRS_PUBLIC in
# the build.  Fall back to xtc.h + all xtc_*.h if the Makefile is
# absent.  Excludes internal-only headers (xtc_sim.h, xtc_tailcall.h,
# *_ext.h) which are not installed.
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

# Extract PUBLIC function names.  The umbrella xtc.h declares its
# version/error functions with plain prototypes (no PUBLIC: marker), so
# scan it for prototypes too; every other header uses PUBLIC: markers.
funcs=""
for h in $hdrs; do
	[ -f "$INC/$h" ] || continue
	m=$(grep -oE 'PUBLIC:[^;]*[ *]xtc_[A-Za-z0-9_]+ __P' "$INC/$h" 2>/dev/null |
	    sed -E 's/.*[ *](xtc_[A-Za-z0-9_]+) __P/\1/')
	if [ "$h" = "xtc.h" ]; then
		m="$m $(grep -oE '^[A-Za-z_].*[ *]xtc_[A-Za-z0-9_]+\(' "$INC/$h" 2>/dev/null |
		    grep -oE 'xtc_[A-Za-z0-9_]+\(' | sed 's/(//')"
	fi
	funcs="$funcs $m"
done
funcs=$(echo "$funcs" | tr ' ' '\n' | grep -E '^xtc_' | sort -u)

if [ -z "$funcs" ]; then
	echo "  [D4] FAIL: no PUBLIC functions extracted from headers" >&2
	exit 1
fi

allman=$(cat "$MANDIR"/*.3 2>/dev/null)
if [ -z "$allman" ]; then
	echo "  [D4] FAIL: no man3 pages found in $MANDIR" >&2
	exit 1
fi

missing=""
n=0
for f in $funcs; do
	n=$((n + 1))
	# Documented if it appears as a whole-word .Nm/.Fn token in a page.
	if ! printf '%s\n' "$allman" |
	    grep -qE "(\.Nm|\.Fn)[ 	]+$f( |,|\$|\")"; then
		missing="$missing $f"
	fi
done

if [ -n "$missing" ]; then
	echo "  [D4] FAIL: PUBLIC functions with no man3 mention:$missing" >&2
	exit 1
fi
echo "  [D4] OK: $n public functions across the installed headers, all mentioned in man3"
