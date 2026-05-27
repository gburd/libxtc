#-
# Copyright (c) 2026, The XTC Project — All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/conformance/w4_mutex/xtc/Makefile
#   Build the W4 mutex contention benchmark binary for the xtc runtime.
#
# Paths (override on the command line if the build tree is elsewhere):
#   XTC_BUILD  — directory containing libxtc.a and the generated Makefile
#   HIST_DIR   — directory containing hist.h / hist.c
#
# Usage:
#   make                       # build ./bench with defaults
#   make clean                 # remove ./bench
#   XTC_BUILD=/other/path make # custom build dir
#
# Note: the mutex benchmark does not use xtc's I/O or TLS subsystems, so
# it does not need to link against liburing, libssl, or libcrypto.  The
# only external dependency beyond libxtc.a is -pthread.

CC ?= cc

XTC_ROOT  ?= ../../../..
XTC_BUILD ?= $(XTC_ROOT)/build_unix
HIST_DIR  ?= ../../include

# Optional: extract the liburing flags from the parent build Makefile in
# case this tree was configured with io_uring enabled.  If the variable
# ends up empty the build still succeeds because libxtc.a is self-contained
# when io_uring is disabled (the default Nix build).
_URING_INC  := $(shell grep -m1 '^CFLAGS' '$(XTC_BUILD)/Makefile' 2>/dev/null \
                    | tr ' ' '\n' | grep -m1 '^-I.*uring' || true)
# Only pull in liburing link flags — skip -lssl / -lcrypto which are unneeded
# here and may not be installed in the build environment.
_URING_LIBS := $(shell grep -m1 '^LIBS' '$(XTC_BUILD)/Makefile' 2>/dev/null \
                    | sed 's/^[^=]*=[[:space:]]*//' \
                    | tr ' ' '\n' | grep 'uring' | tr '\n' ' ' || true)

CFLAGS += -O3 -std=c11 -Wall -Wextra \
           -I$(XTC_ROOT)/src/inc \
           -I$(HIST_DIR) \
           $(_URING_INC)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all clean

all: bench

# Compile main.c and hist.c together; hist.c provides HIST_IMPLEMENTATION
# so main.c need not define it itself.
bench: main.c $(HIST_DIR)/hist.c $(XTC_BUILD)/libxtc.a
	$(CC) $(CFLAGS) $(CPPFLAGS) \
	    -o bench \
	    main.c \
	    $(HIST_DIR)/hist.c \
	    $(XTC_BUILD)/libxtc.a \
	    -pthread \
	    $(_URING_LIBS)

clean:
	rm -f bench
