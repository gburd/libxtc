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

# [D4b] Every XTC_E_* code a man page cites must exist in xtc.h.  xtc_cfg.3
# once documented XTC_E_EXIST, which never existed, so a caller switching on
# it could not compile and one reading the page was told the wrong code.
bad=""
for c in $(cat "$MANDIR"/*.3 "$XTC_SRC_DIR"/man/man7/*.7 2>/dev/null |
    grep -ohE 'XTC_E_[A-Z_]+' | sort -u); do
	grep -qE "\b$c\b" "$INC/xtc.h" || bad="$bad $c"
done
if [ -n "$bad" ]; then
	echo "  [D4b] FAIL: man pages cite error codes absent from xtc.h:$bad" >&2
	exit 1
fi
echo "  [D4b] OK: every XTC_E_* cited in the man pages exists in xtc.h"

# [D4c] Each header PUBLIC: marker's return type must match its XTC_API
# prototype.  The markers are the machine-readable API list (and the
# libxtc.map / symbol gates read them); xtc_cfg_session_bind's once said
# `int` for a function returning xtc_cfg_session_t *.
bad=$(cd "$INC" && for h in xtc*.h; do
	grep -oE 'PUBLIC:[^;]*[ *]xtc_[A-Za-z0-9_]+ __P' "$h" |
	    sed -E 's/^PUBLIC:[[:space:]]*//; s/ __P$//' |
	while IFS= read -r m; do
		name=$(printf '%s' "$m" | grep -oE 'xtc_[A-Za-z0-9_]+$')
		mret=$(printf '%s' "$m" | sed -E "s/$name\$//; s/[[:space:]]+/ /g; s/ \*/*/g; s/^ //; s/ \$//")
		proto=$(grep -hE "^XTC_API[^;(]*[ *]$name[[:space:]]*\(" "$h" | head -1)
		[ -n "$proto" ] || continue
		pret=$(printf '%s' "$proto" | sed -E "s/^XTC_API[[:space:]]+//; s/[ *]?$name[[:space:]]*\(.*//; s/[[:space:]]+/ /g; s/ \*/*/g; s/ \$//")
		case "$proto" in *"*$name"*|*"* $name"*) pret="$pret*";; esac
		pret=$(printf '%s' "$pret" | sed -E 's/\*+/*/g')
		mret=$(printf '%s' "$mret" | sed -E 's/\*+/*/g')
		[ "$mret" = "$pret" ] || echo "$h:$name(marker '$mret' vs prototype '$pret')"
	done
done)
if [ -n "$bad" ]; then
	echo "  [D4c] FAIL: PUBLIC: marker return type disagrees with the prototype:" >&2
	echo "$bad" | sed 's/^/        /' >&2
	exit 1
fi
echo "  [D4c] OK: every PUBLIC: marker's return type matches its prototype"
