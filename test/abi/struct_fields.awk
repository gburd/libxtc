# test/abi/struct_fields.awk -- list "NAME: field field ..." for each
# top-level `typedef struct|union ... { ... } NAME;` in a C header (comments
# stripped, nested braces collapsed, function-pointer members named).  Used
# by test/dist/test_abi_layout.sh to generate the struct-layout table.
# Extract "TYPE: field field ..." for each top-level "typedef struct ... { ... } NAME;"
function strip(l) { gsub(/\/\/.*/, "", l); return l }
{
	l = $0
	if (inc) { if (l ~ /\*\//) { sub(/.*\*\//, "", l); inc = 0 } else next }
	while (l ~ /\/\*.*\*\//) sub(/\/\*[^*]*\*+([^\/*][^*]*\*+)*\//, " ", l)
	if (l ~ /\/\*/) { sub(/\/\*.*/, "", l); inc = 1 }
	l = strip(l)
	if (!in_s && l ~ /^typedef[ \t]+(struct|union)[ \t]*[A-Za-z_0-9]*[ \t]*\{/) { in_s = 1; depth = 0; body = ""; }
	if (in_s) {
		body = body " " l
		n = gsub(/\{/, "{", l); m = gsub(/\}/, "}", l); depth += n - m
		if (depth == 0 && body ~ /\}[ \t]*[A-Za-z_0-9]+[ \t]*;/) { emit(body); in_s = 0 }
	}
}
function emit(b,   name, inner, i, c, d, cur, out, decl, k, parts, p, nm) {
	name = b; sub(/.*\}[ \t]*/, "", name); sub(/[ \t]*;.*/, "", name)
	inner = b; sub(/^[^{]*\{/, "", inner); sub(/\}[^}]*$/, "", inner)
	# collapse nested braces: keep the declarator after them
	out = ""; d = 0
	for (i = 1; i <= length(inner); i++) { c = substr(inner, i, 1)
		if (c == "{") { d++; continue } if (c == "}") { d--; continue }
		if (d == 0) out = out c }
	# remove parenthesised parameter lists of function pointers: (*name)(...)
	while (match(out, /\(\*[ \t]*[A-Za-z_0-9]+[ \t]*\)[ \t]*\([^()]*(\([^()]*\)[^()]*)*\)/)) {
		decl = substr(out, RSTART, RLENGTH); sub(/^\(\*[ \t]*/, "", decl); sub(/[ \t]*\).*/, "", decl)
		out = substr(out, 1, RSTART - 1) " " decl " " substr(out, RSTART + RLENGTH) }
	k = split(out, parts, ";"); res = ""
	for (p = 1; p <= k; p++) { s = parts[p]; gsub(/\[[^]]*\]/, "", s); if (s ~ /^[ \t]*$/) continue
		nn = split(s, cm, ","); for (q = 1; q <= nn; q++) { nm = cm[q]; sub(/[ \t]+$/, "", nm)
			if (q == 1 || 1) { match(nm, /[A-Za-z_][A-Za-z_0-9]*[ \t]*$/); nm = substr(nm, RSTART, RLENGTH); sub(/[ \t]+$/, "", nm) }
			if (nm != "" && nm !~ /^(#|if|endif)/) res = res " " nm } }
	print name ":" res
}
