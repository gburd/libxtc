#!/bin/sh
# test/dist/test_api_reach.sh -- merge gate for PLAN 19.27.15.
#
# Every EXPORTED public function must be referenced by at least one test
# under test/.  "Exported" means what this build actually ships: the
# defined text symbols matching xtc_* in libxtc.a (dist/libxtc.map exports
# exactly xtc_* from the shared library).  Taking the list from the built
# archive, not from header markers, is deliberate: the review that found
# "6 untested functions" scanned PUBLIC: markers only, and missed 24 more
# exported functions whose headers (xtc_fs.h, xtc_tnt.h, the recovery half
# of xtc_proc.h, xtc_sim.h) carry no markers.
#
# A reference is a whole-word mention of the name in a C/C++ source or
# header under test/ with comments stripped (a name that appears only in a
# comment is not tested), or in a test/ shell script (some gates drive the
# API through a compiled here-doc).
#
# The list is per-configuration: a symbol that exists only on another
# platform or backend (the Windows xproc re-exec path, a TLS backend not
# selected here) is simply not in this build's archive, so it is not
# checked here -- it is checked by the build that ships it.
#
# Justified exceptions go in test/dist/api_reach_allow.txt, one per line:
#     <symbol> <reason>
# A stale allowlist entry (a symbol that is now tested, or no longer
# exported) is itself a failure, so the list cannot rot.
#
# Exit 0 = clean, 1 = an untested export (or a stale allowlist line).

set -eu
: "${XTC_SRC_DIR:?}"
: "${XTC_BUILD_DIR:?}"

LIB="$XTC_BUILD_DIR/libxtc.a"
ALLOW="$XTC_SRC_DIR/test/dist/api_reach_allow.txt"

if [ ! -f "$LIB" ]; then
	echo "  [api-reach] FAIL: $LIB not built" >&2
	exit 1
fi
if ! command -v nm >/dev/null 2>&1; then
	echo "  [api-reach] SKIP: nm not on PATH"
	exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp" || true' EXIT

nm --defined-only "$LIB" 2>/dev/null |
    awk '$2 == "T" && $3 ~ /^xtc_/ { print $3 }' | sort -u >"$tmp/exported"
if [ ! -s "$tmp/exported" ]; then
	echo "  [api-reach] FAIL: no exported xtc_* symbols found in $LIB" >&2
	exit 1
fi

# Referenced names: C sources with /* */ and // comments removed (the same
# stripper test_api_discipline.sh uses), plus shell scripts verbatim.
find "$XTC_SRC_DIR/test" -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' \) \
    ! -name 'munit.[ch]' -exec awk '
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
	}' {} + >"$tmp/src"
find "$XTC_SRC_DIR/test" -type f -name '*.sh' -exec cat {} + >>"$tmp/src"
grep -oE '(^|[^A-Za-z0-9_])xtc_[A-Za-z0-9_]+' "$tmp/src" |
    sed 's/^[^x]*//' | sort -u >"$tmp/referenced"

# Allowlist: first field is the symbol; a reason is mandatory.
: >"$tmp/allowed"
bad_allow=""
if [ -f "$ALLOW" ]; then
	while IFS= read -r line; do
		case "$line" in '' | '#'*) continue ;; esac
		sym=${line%%[ 	]*}
		reason=${line#"$sym"}
		if [ -z "$(printf '%s' "$reason" | tr -d ' \t')" ]; then
			bad_allow="$bad_allow $sym(no-reason)"
			continue
		fi
		echo "$sym" >>"$tmp/allowed"
	done <"$ALLOW"
fi
sort -u "$tmp/allowed" -o "$tmp/allowed"

untested=$(comm -23 "$tmp/exported" "$tmp/referenced" |
    comm -23 - "$tmp/allowed")
# Stale: allowlisted but exported AND referenced (now tested -- drop it).
stale=$(comm -12 "$tmp/allowed" "$tmp/exported" |
    comm -12 - "$tmp/referenced")

n_exp=$(wc -l <"$tmp/exported" | tr -d ' ')
n_allow=$(wc -l <"$tmp/allowed" | tr -d ' ')
fail=0
if [ -n "$untested" ]; then
	echo "  [api-reach] FAIL: exported xtc_* function(s) no test references:" >&2
	printf '%s\n' "$untested" | sed 's/^/        /' >&2
	echo "        Add a behavioral test (see test/coverage/test_api_reach.c)," >&2
	echo "        or a justified line in test/dist/api_reach_allow.txt." >&2
	fail=1
fi
if [ -n "$stale" ]; then
	echo "  [api-reach] FAIL: allowlisted but now tested (remove from" >&2
	echo "        test/dist/api_reach_allow.txt):" >&2
	printf '%s\n' "$stale" | sed 's/^/        /' >&2
	fail=1
fi
if [ -n "$bad_allow" ]; then
	echo "  [api-reach] FAIL: allowlist line(s) without a reason:$bad_allow" >&2
	fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "  [api-reach] OK: all $n_exp exported xtc_* functions referenced by a test ($n_allow allowlisted)"
