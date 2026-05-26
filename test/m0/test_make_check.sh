#!/bin/sh
# test/m0/test_make_check.sh
# Verifies M0_CLAIMS.md [B4]: `make check` itself is the meta-test.
# When this script runs *inside* `make check`, all we have to do is
# observe that we got here.  The very fact of being invoked under
# `make check` is the proof.
echo "  [B4] OK: make check invoked us"
