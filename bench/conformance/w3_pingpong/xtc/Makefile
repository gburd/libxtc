#-
# Copyright (c) 2026, The XTC Project — All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/conformance/w3_pingpong/xtc/Makefile
#   Build the W3 mailbox ping-pong benchmark binary for the xtc runtime.
#
# Paths (override on the command line if the build tree is elsewhere):
#   XTC_BUILD   — directory containing libxtc.a and the generated Makefile
#   HIST_DIR    — directory containing hist.h / hist.c
#
# Usage:
#   make                       # build ./bench with defaults
#   make clean                 # remove ./bench
#   XTC_BUILD=/other/path make # custom build dir
#

CC        ?= cc

XTC_ROOT  ?= ../../../..
XTC_BUILD ?= $(XTC_ROOT)/build_unix
HIST_DIR  ?= ../../include

# ---------------------------------------------------------------------------
# Extract the liburing include path and link flags from the xtc build
# Makefile so we do not duplicate Nix store paths here.  Both shell
# snippets run once at parse time; if the xtc Makefile is absent the
# variables stay empty and the linker uses its default search path.
# ---------------------------------------------------------------------------
_URING_INC  := $(shell grep -m1 'CFLAGS' '$(XTC_BUILD)/Makefile' 2>/dev/null \
                    | tr ' ' '\n' | grep -m1 '^-I.*uring')
_URING_LIBS := $(shell grep -m1 'LIBS' '$(XTC_BUILD)/Makefile' 2>/dev/null \
                    | sed 's/^[^=]*=[[:space:]]*//')

CFLAGS  += -O3 -std=c11 -Wall -Wextra \
            -I$(XTC_ROOT)/src/inc \
            -I$(HIST_DIR) \
            $(_URING_INC)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all clean

all: bench

# Compile main.c and hist.c together, linking against libxtc.a and liburing.
# hist.c provides the HIST_IMPLEMENTATION translation unit so main.c does
# not need to define HIST_IMPLEMENTATION itself.
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
