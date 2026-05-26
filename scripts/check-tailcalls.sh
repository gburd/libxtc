#!/bin/sh
#
# scripts/check-tailcalls.sh
#	Validate that source-code call-sites annotated with
#	XTC_MUSTTAIL or XTC_TAIL_CALL() actually compile to a
#	tail-call (jmp on x86_64) and not a regular call+ret.
#
#	Usage: ./scripts/check-tailcalls.sh [build_dir]
#	Default build dir: build_unix
#
#	Approach:
#	  1. Find every "XTC_MUSTTAIL" or "XTC_TAIL_CALL" in src/.
#	  2. For each, identify the enclosing C function name.
#	  3. objdump -d the corresponding .o file and look for
#	     the function's last instruction before its 'ret' /
#	     end-of-function marker.  If it's a 'jmp', good.  If
#	     it's a 'call' followed by 'ret', that's a TCO miss.
#	  4. Report a summary table.
#
#	Limitations:
#	  - x86_64 only for now (other arches use different mnemonics).
#	  - Conservatively warns rather than fails when the toolchain
#	    didn't honor the attribute (e.g. GCC < 15 without musttail).
#	  - Does not work on optimized-out (inlined) functions; those
#	    have no .o symbol to inspect.

set -eu

BUILD_DIR="${1:-build_unix}"
SRC_DIR="src"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$ROOT"

if [ ! -d "$BUILD_DIR" ]; then
	echo "check-tailcalls: $BUILD_DIR not found; run configure+make first" >&2
	exit 1
fi

OBJDUMP="${OBJDUMP:-objdump}"
if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
	echo "check-tailcalls: objdump not in PATH" >&2
	exit 1
fi

ARCH="$(uname -m)"
case "$ARCH" in
	x86_64|amd64) ;;
	*)
		echo "check-tailcalls: arch $ARCH not yet supported (x86_64 only)" >&2
		exit 0
		;;
esac

# Find call sites.  Restrict to xtc source under src/.
sites_file=$(mktemp)
trap 'rm -f "$sites_file"' EXIT

grep -rn "XTC_MUSTTAIL\|XTC_TAIL_CALL" "$SRC_DIR" \
	--include='*.c' --include='*.h' \
	2>/dev/null \
	| grep -v "xtc_tailcall.h\|^Binary" \
	> "$sites_file" || true

n_sites=$(wc -l < "$sites_file" | awk '{print $1}')

if [ "$n_sites" -eq 0 ]; then
	echo "check-tailcalls: no annotated sites found"
	exit 0
fi

ok=0
warn=0
fail=0
unverified=0

echo "check-tailcalls: scanning $n_sites annotated sites in $SRC_DIR/"
echo ""
printf "%-50s %-30s %s\n" "FILE:LINE" "FUNCTION" "RESULT"
echo "------------------------------------------------------------------------------------------------"

while IFS=: read -r file lineno _; do
	[ -n "$file" ] || continue
	# Identify the enclosing function: scan backwards from $lineno
	# for the last line matching the function-definition pattern
	# `^[a-z_][a-zA-Z_0-9 *]*(`.
	fn=$(awk -v upto="$lineno" '
		NR > upto { exit }
		/^[a-zA-Z_][a-zA-Z_0-9 \*]*\([^)]*\)/ { last = $0 }
		END {
			# Extract the function name.
			s = last
			sub(/\(.*$/, "", s)
			# Take the last whitespace-separated token.
			n = split(s, parts, /[ \t\*]+/)
			if (n > 0) print parts[n]
		}
	' "$file")
	[ -z "$fn" ] && fn="<unknown>"

	# Find the .o for this source.
	src_base=$(basename "$file" .c)
	obj=$(find "$BUILD_DIR" -maxdepth 2 -name "${src_base}.o" 2>/dev/null \
		| head -1)
	if [ -z "$obj" ]; then
		printf "%-50s %-30s %s\n" "$file:$lineno" "$fn" \
			"unverified (.o not found; inlined?)"
		unverified=$((unverified + 1))
		continue
	fi

	# Disassemble and find the function.
	disasm=$($OBJDUMP -d --no-show-raw-insn -M intel "$obj" 2>/dev/null \
		| awk -v fn="$fn" '
			/^[0-9a-f]+ <.*>:$/ {
				name = $2
				sub(/^</, "", name); sub(/>:$/, "", name)
				cur = (name == fn) ? 1 : 0
				next
			}
			/^$/ { cur = 0 }
			cur { print }
		')

	if [ -z "$disasm" ]; then
		printf "%-50s %-30s %s\n" "$file:$lineno" "$fn" \
			"unverified (no symbol in .o)"
		unverified=$((unverified + 1))
		continue
	fi

	# Look at the last call-or-jmp before 'ret' in the function.
	last_ctrl=$(echo "$disasm" | awk '
		/[[:space:]](call|jmp|ret)([[:space:]]|$)/ { last = $0 }
		END { print last }
	')

	if echo "$last_ctrl" | grep -qE '[[:space:]]jmp([[:space:]]|$)'; then
		printf "%-50s %-30s %s\n" "$file:$lineno" "$fn" \
			"OK (jmp)"
		ok=$((ok + 1))
	elif echo "$last_ctrl" | grep -qE '[[:space:]]ret([[:space:]]|$)'; then
		# Function ends with ret.  Check if there's a call
		# immediately before — that's a TCO miss.
		prev=$(echo "$disasm" | awk '
			/[[:space:]](call|jmp)([[:space:]]|$)/ { last = $0 }
			END { print last }
		')
		if echo "$prev" | grep -qE '[[:space:]]call([[:space:]]|$)'; then
			printf "%-50s %-30s %s\n" "$file:$lineno" "$fn" \
				"WARN (call+ret, no TCO)"
			warn=$((warn + 1))
		else
			printf "%-50s %-30s %s\n" "$file:$lineno" "$fn" \
				"unverified (no terminal call)"
			unverified=$((unverified + 1))
		fi
	else
		printf "%-50s %-30s %s\n" "$file:$lineno" "$fn" \
			"unverified"
		unverified=$((unverified + 1))
	fi
done < "$sites_file"

echo ""
echo "summary: $ok ok, $warn warn, $fail fail, $unverified unverified"

if [ "$fail" -gt 0 ]; then
	exit 1
fi
exit 0
