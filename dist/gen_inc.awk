# dist/gen_inc.awk
# Parse C source for PUBLIC: markers and emit prototype declarations.
# Adapted from BDB/DBSQL.  Reads from stdin or a file, writes to the
# file given by -v i_pfile=...
#
# Recognized lines:
#     PUBLIC: <prototype>;
# Multi-line prototypes may span using the BDB convention where the
# trailing ");" closes the declaration.
#
# Both BDB-style (PUBLIC: in a standalone comment block) and inline
# (/* PUBLIC: prototype; */ on one line) are accepted; in the inline
# case we strip the trailing " */".
#
# Duplicate prototypes (e.g. when two backend .c files export the same
# symbol) are deduped: only the first occurrence is emitted.

/PUBLIC:/ {
	sub("^.*PUBLIC:[ \t]*", "")
	sub("[ \t]*\\*/[ \t]*$", "")
	if ($0 ~ /^#if|^#ifdef|^#ifndef|^#else|^#endif/) {
		if (!(($0) in seen)) {
			print $0 >> i_pfile
			seen[$0] = 1
		}
		next
	}
	pline = pline " " $0
	if (pline ~ /\);/) {
		sub("^[ \t]*", "", pline)
		if (!((pline) in seen)) {
			print pline >> i_pfile
			seen[pline] = 1
		}
		pline = ""
	}
}
