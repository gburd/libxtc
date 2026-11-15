#!/bin/sh
# test/dist/test_install_headers.sh
#
#	Regression guard for the install header surface (1.x blocker B1):
#	`make install` must ship enough headers that a consumer can
#	`#include <xtc_loop.h>` / `<xtc_proc.h>` / ... and link -lxtc.
#	Historically only the xtc.h version/error stub was installed, so
#	the documented install path could not compile the README example.
#
#	This builds the library out-of-source, installs it into a temp
#	prefix, then compiles a consumer that uses the functional API
#	against ONLY the installed headers (-I$prefix/include) -- not the
#	in-tree src/inc, which would mask a missing install.

set -eu

XTC_SRC_DIR="${XTC_SRC_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
CC="${CC:-cc}"

work="$(mktemp -d)"
trap 'cd / 2>/dev/null; rm -rf "$work" || true' EXIT INT TERM

prefix="$work/prefix"
build="$work/build"
mkdir -p "$build"

# Configure must already be generated (autoreconf run by make check's
# environment); if dist/configure is absent, skip rather than fail.
if [ ! -x "$XTC_SRC_DIR/dist/configure" ]; then
	echo "  [install-headers] SKIP: dist/configure not generated"
	exit 0
fi

(
	cd "$build"
	"$XTC_SRC_DIR/dist/configure" --prefix="$prefix" >/dev/null 2>&1
	make -j"$(nproc 2>/dev/null || echo 2)" >/dev/null 2>&1
	"${MAKE:-make}" install >/dev/null 2>&1
) || { echo "  [install-headers] FAIL: build/install errored"; exit 1; }

# The umbrella header must exist.
test -f "$prefix/include/xtc.h" || {
	echo "  [install-headers] FAIL: xtc.h not installed"; exit 1; }

# A representative spread of functional headers must be installed.
for h in xtc_loop.h xtc_proc.h xtc_chan.h xtc_net.h xtc_aio.h \
         xtc_sync.h xtc_lrlock.h xtc_res.h xtc_app.h xtc_svr.h; do
	test -f "$prefix/include/$h" || {
		echo "  [install-headers] FAIL: $h not installed"; exit 1; }
done

# Compile a consumer that uses the functional API against the INSTALLED
# headers only.  This is the README's 30-second example shape.
cat > "$work/consumer.c" <<'EOF'
#include <xtc.h>
#include <xtc_loop.h>
#include <xtc_proc.h>
#include <xtc_chan.h>
#include <xtc_net.h>
#include <xtc_aio.h>
#include <stddef.h>

int
main(void)
{
	xtc_loop_t *loop = NULL;
	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	(void)xtc_aio_fsync;          /* a real symbol, exercised below */
	(void)xtc_proc_spawn;
	(void)xtc_chan_mpsc_create;
	(void)xtc_net_listen;
	xtc_loop_fini(loop);
	return 0;
}
EOF

# Pull link flags from the installed pkg-config so the test does not
# hardcode -luring/-lssl ordering (static link pulls the private deps).
# Export PKG_CONFIG_PATH (some pkg-config wrappers ignore an inline
# assignment) and prepend it so a system xtc.pc cannot shadow ours.
PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists xtc 2>/dev/null; then
	cflags="$(pkg-config --cflags xtc)"
	libs="$(pkg-config --libs --static xtc)"
else
	# Fallback: read Libs.private straight out of the installed .pc so
	# the private deps (-luring/-lssl/-pthread) are still linked even
	# where pkg-config is unavailable or uncooperative.
	priv=""
	if [ -f "$prefix/lib/pkgconfig/xtc.pc" ]; then
		priv="$(sed -n 's/^Libs.private:[[:space:]]*//p' "$prefix/lib/pkgconfig/xtc.pc")"
	fi
	cflags="-I$prefix/include"
	libs="-L$prefix/lib -lxtc $priv -lpthread"
fi

# cflags/libs intentionally word-split (each holds several flags).
# shellcheck disable=SC2086
if ! $CC -std=c11 -D_GNU_SOURCE $cflags "$work/consumer.c" \
	$libs -o "$work/consumer" 2> "$work/cc.err"; then
	echo "  [install-headers] FAIL: consumer did not compile/link against the installed headers"
	head -20 "$work/cc.err" >&2
	exit 1
fi

# Every installed header must be self-contained: compiles as the FIRST
# and ONLY include (a header that leans on a type its own includes do
# not provide -- e.g. size_t without <stddef.h> -- breaks a consumer
# whose include order differs from ours).
bad=0
for h in "$prefix"/include/xtc*.h; do
	b=$(basename "$h")
	printf '#include <%s>\nint main(void) { return 0; }\n' "$b" > "$work/one.c"
	if ! $CC -std=c11 -D_GNU_SOURCE $cflags -fsyntax-only "$work/one.c" \
	    2> "$work/one.err"; then
		echo "  [install-headers] FAIL: $b is not self-contained"
		head -3 "$work/one.err" >&2
		bad=1
	fi
done
[ $bad -eq 0 ] || exit 1

# The simulation library: a sim build installs libxtc-sim.a and
# xtc-sim.pc into the same prefix, and an application's own DST test
# (docs/_includes/snippets/20_dst_app.c, xtc_sim.h only) builds against
# the installed headers with `pkg-config xtc-sim` and runs its seeds.
sbuild="$work/build-sim"
mkdir -p "$sbuild"
if ! (
	cd "$sbuild"
	"$XTC_SRC_DIR/dist/configure" --prefix="$prefix" \
	    --with-io-backend=sim --with-tls=none >/dev/null 2>&1
	make -j"$(nproc 2>/dev/null || echo 2)" libxtc.a xtc-sim.pc \
	    >/dev/null 2>&1
	"${MAKE:-make}" install >/dev/null 2>&1
); then
	echo "  [install-headers] FAIL: the sim build did not build or install"
	exit 1
fi
for f in lib/libxtc-sim.a lib/libxtc.a lib/pkgconfig/xtc-sim.pc \
    lib/pkgconfig/xtc.pc include/xtc_sim.h; do
	test -f "$prefix/$f" || {
		echo "  [install-headers] FAIL: $f not installed"; exit 1; }
done
if command -v pkg-config >/dev/null 2>&1 &&
    pkg-config --exists xtc-sim 2>/dev/null; then
	scflags="$(pkg-config --cflags xtc-sim)"
	slibs="$(pkg-config --libs --static xtc-sim)"
else
	spriv="$(sed -n 's/^Libs.private:[[:space:]]*//p' \
	    "$prefix/lib/pkgconfig/xtc-sim.pc")"
	scflags="-I$prefix/include"
	slibs="-L$prefix/lib -lxtc-sim $spriv -lpthread"
fi
# shellcheck disable=SC2086
if ! $CC -std=c11 -D_GNU_SOURCE $scflags \
    "$XTC_SRC_DIR/docs/_includes/snippets/20_dst_app.c" $slibs \
    -o "$work/dst_app" 2> "$work/cc.err"; then
	echo "  [install-headers] FAIL: a DST test did not build against" \
	    "the installed xtc-sim"
	head -20 "$work/cc.err" >&2
	exit 1
fi
if ! "$work/dst_app" > "$work/dst_app.out" 2>&1 ||
    ! grep -q 'seeds, each replayed' "$work/dst_app.out"; then
	echo "  [install-headers] FAIL: the installed-xtc-sim DST test failed:"
	cat "$work/dst_app.out" >&2
	exit 1
fi

echo "  [install-headers] OK: consumer compiles + links against the installed public headers; each header is self-contained; an application DST test builds against the installed xtc-sim and replays"
exit 0
