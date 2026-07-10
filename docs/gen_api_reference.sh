#!/bin/sh
# docs/gen_api_reference.sh -- generate the in-site API reference from the
# man pages.  Renders each man/man{3,7}/*.{3,7} to an HTML fragment with
# mandoc and emits categorized Jekyll pages under docs/reference/api/.
#
# Run from the repo root (or via `make docs-api`).  Re-run whenever a man
# page changes; the Pages workflow runs it before the Jekyll build so the
# site is always current.  Requires mandoc.
set -eu

here=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(unset CDPATH; cd -- "$here/.." && pwd)
MAN="$ROOT/man"
OUT="$here/reference/api"

if ! command -v mandoc >/dev/null 2>&1; then
	echo "gen_api_reference: mandoc not found; skipping" >&2
	exit 0
fi

mkdir -p "$OUT"

# Categories: each is "slug|Title|space-separated man3 basenames (no .3)".
# A page not listed in any category lands in "other".
cat_defs='
basics|Foundational basics|xtc_free xtc_stats xtc_log xtc_strerror xtc_cfg xtc_version_string xtc_version_components
event|Event runtime (L2)|xtc_loop xtc_exec xtc_async
proc|Processes and messaging (L3)|xtc_proc xtc_chan xtc_svr xtc_osproc xtc_stream
sync|Synchronization and locks (L3)|xtc_sync xtc_lwlock xtc_lrlock xtc_lockmgr xtc_rcu xtc_credit
orc|Orchestration (L4)|xtc_supervisor xtc_app xtc_reg xtc_launch xtc_tnt xtc_fsm xtc_pg xtc_pool xtc_xproc
io|I/O, files, and network (L1)|xtc_io xtc_aio xtc_net xtc_fs xtc_bdev xtc_tls xtc_pkey xtc_iosched xtc_dio_sched
mem|Memory and resources|xtc_slab xtc_mctx xtc_res xtc_blocking
obs|Observability and debugging|xtc_inspect xtc_trace xtc_dump xtc_pdict xtc_alloc_audit
test|Testing and fault injection|xtc_inject xtc_preempt xtc_stack_reclaim
'

# Render one man file to an HTML fragment on stdout.
render() { mandoc -T html -O 'fragment' "$1" 2>/dev/null; }

# Track which man3 pages we placed so "other" can catch the rest.
placed=" "

emit_page() {
	# $1 basename (e.g. xtc_loop)  $2 section (3|7)  $3 nav_parent  $4 nav_order
	base=$1 sec=$2 parent=$3 order=$4
	f="$MAN/man$sec/$base.$sec"
	[ -f "$f" ] || return 0
	nd=$(grep -m1 '^.Nd ' "$f" | sed 's/^\.Nd //')
	{
		echo '---'
		echo "title: $base($sec)"
		echo "parent: $parent"
		echo "grand_parent: API reference"
		echo "nav_order: $order"
		echo "permalink: /reference/api/$base/"
		echo '---'
		echo
		echo "# ${base}($sec)"
		echo
		[ -n "$nd" ] && { echo "$nd"; echo; }
		echo '<div class="manpage" markdown="0">'
		render "$f"
		echo '</div>'
		echo
		echo "[View the mdoc source](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man$sec/$base.$sec){: .fs-2 }"
	} > "$OUT/$base.md"
}

# Category index pages + their member function pages.
order=1
echo "$cat_defs" | while IFS='|' read -r slug title members; do
	[ -n "$slug" ] || continue
	# category landing page
	{
		echo '---'
		echo "title: $title"
		echo 'parent: API reference'
		echo "nav_order: $order"
		echo "permalink: /reference/api/$slug/"
		echo 'has_children: true'
		echo '---'
		echo
		echo "# $title"
		echo
		for m in $members; do
			ndf="$MAN/man3/$m.3"
			[ -f "$ndf" ] || continue
			nd=$(grep -m1 '^.Nd ' "$ndf" | sed 's/^\.Nd //')
			echo "- [\`$m(3)\`]({{ '/reference/api/$m/' | relative_url }}) -- $nd"
		done
	} > "$OUT/cat-$slug.md"
	sub=1
	for m in $members; do
		emit_page "$m" 3 "$title" "$sub"
		sub=$((sub + 1))
	done
	order=$((order + 1))
done

echo "gen_api_reference: wrote pages under $OUT"
