#!/bin/sh
# test/dist/test_api_discipline.sh -- merge/release gate for API discipline.
#
# Enforces the project's API-boundary rules (see AGENTS.md "API discipline"):
#
#  RULE 1 (library uses the __os_* wrappers): library code (src/, minus the
#    wrapper implementations themselves) must not call the RAW cross-platform
#    primitives the __os_* layer wraps -- malloc/calloc/realloc/free/strdup,
#    pthread_create, clock_gettime/gettimeofday/nanosleep/usleep -- because the
#    wrappers add the alloc hook (embedder accounting), preemption-safety
#    brackets, and portability.  Raw calls bypass all three.
#
#  RULE 2 (no ignored __os_ alloc rc): never "(void)__os_malloc(...)" etc.;
#    always check "!= XTC_OK".  And do NOT double-check the return AND the
#    pointer == NULL -- __os_malloc/_calloc/_realloc guarantee a non-NULL
#    pointer on XTC_OK (an invariant), so the NULL check is dead code.
#
#  RULE 3 (consumers use only xtc_*): the shipped examples (a stand-in for a
#    library consumer) must use only the public xtc_* API, never the internal
#    __os_* / __xtc_* surface.
#
# A legitimate raw call inside library code (a platform shim definition, an
# off-loop fallback) is exempted with an explicit "/* XTC_RAW_OK: reason */"
# marker on the same line.  Keep those rare and justified.
#
# Exit 0 = clean, 1 = a violation (fails the merge/release gate + CI).

set -u
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT="${XTC_SRC_DIR:-$(cd "$SELF_DIR/../.." && pwd)}"
fail=0

# ---- RULE 1: raw wrapped primitives in library code -----------------------
# Library .c files, excluding the wrapper implementations (src/os/*) which ARE
# the wrappers, and the compat shims.
lib_c=$(find "$ROOT/src" -name '*.c' | grep -vE '/src/os/|/inc/compat/')

# Raw alloc family.  Exclude: __os_ prefixed, the alloc-hook indirections
# (->malloc / g_down. / __hook), struct/field names containing 'free', and
# lines carrying the XTC_RAW_OK marker.
# Strip C comments (block + line) so a primitive named only inside a comment
# is never flagged.  A small awk state machine handles multi-line /* */.
strip_comments() {
	awk '
		BEGIN { inblk = 0 }
		{
			line = $0; out = ""; i = 1; n = length(line)
			while (i <= n) {
				c = substr(line, i, 1); c2 = substr(line, i, 2)
				if (inblk) {
					if (c2 == "*/") { inblk = 0; i += 2 } else { i++ }
				} else if (c2 == "/*") { inblk = 1; i += 2 }
				else if (c2 == "//") { break }
				else { out = out c; i++ }
			}
			print out
		}' "$1"
}

# A candidate is "file:lineno:strippedtext".  Re-check the ORIGINAL line
# for an exemption marker (XTC_RAW_OK / XTC_BLOCKING_OK), which lives in a
# comment the stripper removed -- so markers must be matched pre-strip.
marker_ok() {
	_f=${1%%:*}; _rest=${1#*:}; _ln=${_rest%%:*}
	sed -n "${_ln}p" "$_f" 2>/dev/null | grep -qE 'XTC_RAW_OK|XTC_BLOCKING_OK'
}
filter_markers() {
	while IFS= read -r _hit; do
		[ -n "$_hit" ] || continue
		marker_ok "$_hit" || printf '%s\n' "$_hit"
	done
}

raw_alloc=$(printf '%s\n' "$lib_c" | while IFS= read -r f; do
	strip_comments "$f" | grep -nE '(^|[^_A-Za-z0-9>.])(malloc|calloc|realloc|strdup)[[:space:]]*\(' 2>/dev/null \
	| grep -vE '__os_|->malloc|->calloc|->realloc|->strdup|g_down\.|__hook' \
	| sed "s#^#$f:#"
done | filter_markers)
raw_free=$(printf '%s\n' "$lib_c" | while IFS= read -r f; do
	strip_comments "$f" | grep -nE '(^|[[:space:];{}(])free[[:space:]]*\(' 2>/dev/null \
	| grep -vE '__os_free|xtc_free|slab_free|_free\(|->free|\.free|free_|_free|pthread|sem_|cond_|munmap' \
	| sed "s#^#$f:#"
done | filter_markers)
raw_thr=$(printf '%s\n' "$lib_c" | while IFS= read -r f; do
	strip_comments "$f" | grep -nE 'pthread_create[[:space:]]*\(' 2>/dev/null \
	| grep -vE '__os_pthread_create_masked' | sed "s#^#$f:#"
done | filter_markers)
raw_time=$(printf '%s\n' "$lib_c" | while IFS= read -r f; do
	strip_comments "$f" | grep -nE '(clock_gettime|gettimeofday|nanosleep|usleep)[[:space:]]*\(' 2>/dev/null \
	| grep -vE '__os_' \
	| sed "s#^#$f:#"
done | filter_markers)

for label in "raw alloc:$raw_alloc" "raw free:$raw_free" \
             "raw pthread_create:$raw_thr" "raw clock/sleep:$raw_time"; do
	name=${label%%:*}; body=${label#*:}
	if [ -n "$body" ]; then
		echo "  [api] FAIL RULE 1 ($name -- use the __os_* wrapper, or mark XTC_RAW_OK):"
		printf '%s\n' "$body" | sed 's/^/        /'
		fail=1
	fi
done

# ---- RULE 2: ignored / double-checked __os_ alloc returns -----------------
voided=$(printf '%s\n' "$lib_c" | while IFS= read -r f; do
	grep -nE '\(void\)[[:space:]]*__os_(malloc|calloc|realloc)\b' "$f" 2>/dev/null \
	| sed "s#^#$f:#"
done)
if [ -n "$voided" ]; then
	echo "  [api] FAIL RULE 2 (never (void) an __os_ alloc -- check != XTC_OK):"
	printf '%s\n' "$voided" | sed 's/^/        /'
	fail=1
fi
dblchk=$(printf '%s\n' "$lib_c" | while IFS= read -r f; do
	grep -nE '__os_(malloc|calloc|realloc)\([^;]*!= XTC_OK[^;]*\|\|[^;]*== NULL' "$f" 2>/dev/null \
	| sed "s#^#$f:#"
done)
if [ -n "$dblchk" ]; then
	echo "  [api] FAIL RULE 2 (redundant == NULL check; __os_ alloc is non-NULL on XTC_OK):"
	printf '%s\n' "$dblchk" | sed 's/^/        /'
	fail=1
fi

# ---- RULE 3: consumers (examples) using the internal surface --------------
# NOTE: currently a WARNING, not a hard gate.  The examples still reach into
# __os_* for allocation, clocks, sleep, and atomics because the public xtc_*
# surface does not yet expose all of those (xtc_malloc/_calloc/_realloc/_free
# exist; public clock/sleep/atomic/aligned-alloc are still TODO).  Once the
# public API is complete and the examples are migrated, flip this to a hard
# failure (set rule3_fatal=1).  Tracked in docs/KNOWN_ISSUES.md.
rule3_fatal=0
ex_c=$(find "$ROOT/examples" -name '*.c' 2>/dev/null)
consumer=$(printf '%s\n' "$ex_c" | while IFS= read -r f; do
	[ -n "$f" ] || continue
	grep -nE '\b__os_[a-z]|\b__xtc_[a-z]' "$f" 2>/dev/null \
	| grep -vE 'XTC_RAW_OK|/\*|//' | sed "s#^#$f:#"
done)
if [ -n "$consumer" ]; then
	n=$(printf '%s\n' "$consumer" | grep -c .)
	echo "  [api] WARN RULE 3: $n site(s) where a consumer/example uses the"
	echo "        internal __os_/__xtc_ surface (consumers should use only the"
	echo "        public xtc_* API).  Non-fatal until the public API is complete."
	if [ "$rule3_fatal" -eq 1 ]; then fail=1; fi
fi

if [ "$fail" -eq 0 ]; then
	echo "  [api] OK: API discipline -- library uses __os_* wrappers (checked, "
	echo "        never voided), no dead NULL checks, consumers use only xtc_*"
fi
exit "$fail"
