#!/bin/sh
# test/dist/test_s_cfg.sh -- verify dist/s_cfg passes (or notes
# absence of docs/cfg.md).
set -eu

DIST_DIR="${XTC_SRC_DIR:?}/dist"
exec "$DIST_DIR/s_cfg"
