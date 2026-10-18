#!/bin/bash
# test/sqlxtc/test_sqlxtc_tpcc.sh
# TPROC-C differential test: the Turso harness's 35 prepared statements
# (bound ? params over Quack) and HammerDB quackoltp.tcl's statements,
# each on its driver's schema, run through sqlxtc and python sqlite3 in
# lockstep; every result (and the UNIQUE error text) must match.

XTC_SRC_DIR="${XTC_SRC_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
SERVER="$XTC_SRC_DIR/examples/06_sqlxtc/sqlxtc-server"

if [ ! -x "$SERVER" ]; then
	echo "  [sqlxtc-tpcc] SKIP: $SERVER not built"
	exit 0
fi
if ! python3 -c "import sqlite3" 2>/dev/null; then
	echo "  [sqlxtc-tpcc] SKIP: python3 with sqlite3 not available"
	exit 0
fi

cd "$XTC_SRC_DIR"
python3 "$XTC_SRC_DIR/test/sqlxtc/test_sqlxtc_tpcc.py" > /tmp/sqlxtc-tpcc.$$ 2>&1
rc=$?
grep -E -A4 "FAIL" /tmp/sqlxtc-tpcc.$$ | head -60
grep -E "^tpcc|^TPCC" /tmp/sqlxtc-tpcc.$$
rm -f /tmp/sqlxtc-tpcc.$$
exit $rc
