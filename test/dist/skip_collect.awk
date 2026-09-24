# test/dist/skip_collect.awk -- PLAN 19.27.21: pull SKIP evidence out of ONE
# test's output, for the end-of-`make check` SKIP SUMMARY.
#
#   awk -v t=<test name> -v rc=<exit status> -f skip_collect.awk <output>
#
# Prints one "<test>: <what was skipped / why>" line per skip found:
#   - munit cases that returned MUNIT_SKIP ("[ SKIP  ]" result).  munit
#     prints no reason, so the case path is the pointer into the source.
#     The case name is the last "/suite/case" line seen, because a case
#     that writes output can push its result onto a later line;
#   - any line with the uppercase WORD SKIP, not SKIPPED (the
#     "SKIP: reason" / "[tag] SKIP:" / "[PBT] x SKIP (...)" convention
#     every other tier uses);
#   - exit 77 with no reason printed.
# It only reads; it never affects the test's exit status.
BEGIN { name = ""; found = 0 }
/^\/[A-Za-z0-9_]/ { name = $1; sub(/\[.*/, "", name) }
/\[ SKIP +\]/ {
	printf "%s: %s (munit case returned MUNIT_SKIP)\n", t, name
	found = 1
	next
}
/(^|[^A-Za-z])SKIP([^A-Za-z]|$)/ {
	line = $0
	sub(/^[ \t]+/, "", line)
	printf "%s: %s\n", t, line
	found = 1
}
END {
	if (rc == 77 && !found)
		printf "%s: exited 77 (skip) without printing a reason\n", t
}
