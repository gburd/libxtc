#!/bin/sh
# test/dist/test_s_noalloc.sh -- verify dist/s_noalloc passes on the
# tree (i.e., no unjustified allocation-symbol reference inside an
# XTC_NOALLOC_BEGIN/END hot-path region, and every region is balanced).
set -eu

DIST_DIR="${XTC_SRC_DIR:?}/dist"
exec "$DIST_DIR/s_noalloc"
