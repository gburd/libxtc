#!/bin/sh
# test/dist/test_s_layer.sh -- verify dist/s_layer passes on the tree
# (i.e., no .c file includes a header owned by a strictly-higher layer,
# except the documented XTC_LAYER_OK exceptions).
set -eu

DIST_DIR="${XTC_SRC_DIR:?}/dist"
exec "$DIST_DIR/s_layer"
