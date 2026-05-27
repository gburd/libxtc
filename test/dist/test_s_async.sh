#!/bin/sh
# test/dist/test_s_async.sh -- verify dist/s_async passes on the
# tree (i.e., no unjustified blocking calls outside the allowed
# layers).
set -eu

DIST_DIR="${XTC_SRC_DIR:?}/dist"
exec "$DIST_DIR/s_async"
