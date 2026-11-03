#!/bin/sh
#
# test/cbmc/check_drift.sh -- a transcribed CBMC harness must still match
# the source it claims to model.
#
# A harness that cannot compile the real file carries the transcribed
# statements between two markers:
#
#	/* BEGIN SOURCE <path> <label> */
#	...statements copied from <path>...
#	/* END SOURCE */
#
# For every such block this script checks that the block's lines, in
# order, occur in <path> as one contiguous run, comparing each line with
# all whitespace removed (so re-indenting the copy is fine, changing a
# token is not).  Lines inside #if/#ifdef/#ifndef ... #endif in the
# harness that select a planted bug are skipped (see below), and so are
# harness-only lines tagged `/* harness */`.
#
# Needs only POSIX sh + awk; runs in make check (no CBMC needed).
# Comments are ignored on both sides.  Preprocessor lines in the block are
# skipped; the lines under `#ifdef PLANT_...` (a planted-bug variant) are
# skipped, the lines under `#ifndef PLANT_...` (real code a plant removes)
# are compared.
#
# Exit 0 = every block found; 1 = a block drifted (the harness proves an
# algorithm the code no longer runs) or names a missing file.

set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$DIR/../.." && pwd)
rc=0
nblk=0

for h in "$DIR"/*_harness.c; do
	[ -f "$h" ] || continue
	grep -qE "^[[:space:]]*/\\* BEGIN SOURCE " "$h" || continue
	# Emit "path<TAB>label<TAB>startline" per block, then the block body
	# to a temp file per block.
	tmp=$(mktemp -d "${TMPDIR:-/var/tmp}/cbmc-drift.XXXXXX")
	awk -v out="$tmp" '
	function strip(l,   o, i, c2) {
		o = ""
		while (length(l) > 0) {
			if (incom) {
				i = index(l, "*/")
				if (i == 0) return o
				l = substr(l, i + 2); incom = 0
			} else {
				i = index(l, "/*"); c2 = index(l, "//")
				if (c2 > 0 && (i == 0 || c2 < i)) { o = o substr(l, 1, c2 - 1); return o }
				if (i == 0) { o = o l; return o }
				o = o substr(l, 1, i - 1); l = substr(l, i + 2); incom = 1
			}
		}
		return o
	}
	/^[ \t]*\/\* BEGIN SOURCE [^ ]+/ {
		n++; inb = 1; depth = 0
		line = $0
		sub(/.*BEGIN SOURCE[ \t]+/, "", line); sub(/[ \t]*\*\/.*/, "", line)
		print line > (out "/" n ".hdr"); close(out "/" n ".hdr")
		next
	}
	/^[ \t]*\/\* END SOURCE \*\// { inb = 0; close(out "/" n ".body"); next }
	inb {
		if ($0 ~ /\/\* harness \*\//) next
		if ($0 ~ /^[ \t]*#[ \t]*ifdef[ \t]+PLANT_/ ||
		    $0 ~ /^[ \t]*#[ \t]*if[ \t]+defined\(PLANT_/) {
			st[++sp] = skip; skip = 1; next
		}
		if ($0 ~ /^[ \t]*#[ \t]*ifndef[ \t]+PLANT_/) {
			st[++sp] = skip; plantneg[sp] = 1; next
		}
		if ($0 ~ /^[ \t]*#[ \t]*if/) { st[++sp] = skip; next }
		if ($0 ~ /^[ \t]*#[ \t]*else/) {
			if (sp > 0 && plantneg[sp]) skip = 1
			else if (sp > 0) skip = st[sp]
			next
		}
		if ($0 ~ /^[ \t]*#[ \t]*endif/) {
			if (sp > 0) { skip = st[sp]; plantneg[sp] = 0; sp-- }
			next
		}
		if ($0 ~ /^[ \t]*#/) next
		if (skip) next
		l = strip($0); gsub(/[ \t\r]/, "", l)
		if (l == "") next
		print l > (out "/" n ".body")
	}' "$h"
	for hdr in "$tmp"/*.hdr; do
		[ -f "$hdr" ] || continue
		nblk=$((nblk + 1))
		b=${hdr%.hdr}.body
		set -- $(cat "$hdr")
		src=$1; shift; label="$*"
		name=$(basename "$h" .c)
		if [ ! -f "$TOP/$src" ]; then
			echo "  [drift] FAIL: $name: $src (block '$label') does not exist"
			rc=1; continue
		fi
		if awk -v want="$b" '
	function strip(l,   o, i, c2) {
		o = ""
		while (length(l) > 0) {
			if (incom) {
				i = index(l, "*/")
				if (i == 0) return o
				l = substr(l, i + 2); incom = 0
			} else {
				i = index(l, "/*"); c2 = index(l, "//")
				if (c2 > 0 && (i == 0 || c2 < i)) { o = o substr(l, 1, c2 - 1); return o }
				if (i == 0) { o = o l; return o }
				o = o substr(l, 1, i - 1); l = substr(l, i + 2); incom = 1
			}
		}
		return o
	}
		BEGIN { while ((getline l < want) > 0) w[++nw] = l }
		{ l = strip($0); gsub(/[ \t\r]/, "", l); if (l == "") next; s[++ns] = l }
		END {
			for (i = 1; i + nw - 1 <= ns; i++) {
				for (j = 1; j <= nw; j++) if (s[i + j - 1] != w[j]) break
				if (j > nw) exit 0
			}
			exit 1
		}' "$TOP/$src"; then
			echo "  [drift] OK: $name: $src ($label)"
		else
			echo "  [drift] FAIL: $name: $src ($label) no longer matches"
			echo "          the harness proves code the source no longer runs;"
			echo "          update the transcription (and re-run CBMC) deliberately"
			rc=1
		fi
	done
	find "$tmp" -type f -delete; rmdir "$tmp"
done
echo "  [drift] $nblk transcribed block(s) checked"
exit $rc
