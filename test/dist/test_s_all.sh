#!/bin/sh
# test/dist/test_s_all.sh -- T1
set -eu
: "${XTC_SRC_DIR:?}"
cd "$XTC_SRC_DIR"
chmod +x dist/s_*
dist/s_all >/dev/null
echo "  [T1] OK: s_all completed"
