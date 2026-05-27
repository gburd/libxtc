#-
# Copyright (c) 2026, The XTC Project — All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/conformance/w2_echo/xtc/Makefile
#   Builds the W2 xtc benchmark binary ("bench").
#
# Usage:
#   make                       # build ./bench (defaults)
#   make clean
#   XTC_BUILD=/path make       # override build dir
#
# Link flags are extracted from the parent build's Makefile so we
# do not hard-code Nix store paths here.

XTC_ROOT  ?= ../../../..
XTC_BUILD ?= $(XTC_ROOT)/build_unix
XTC_INC   ?= $(XTC_ROOT)/src/inc
HIST_DIR  ?= ../../include

CC        ?= cc

# ---------------------------------------------------------------------------
# Extract link paths and libraries from the xtc build Makefile.
# We need LDFLAGS (e.g. -L/nix/store/.../openssl/lib) and
# LIBS (e.g. -lssl -lcrypto) or liburing equivalents, whichever
# the current build uses.
# ---------------------------------------------------------------------------
_BUILD_LDFLAGS := $(shell \
    awk '/^LDFLAGS[[:space:]]*=/{sub(/^LDFLAGS[[:space:]]*=[[:space:]]*/,""); print}' \
    '$(XTC_BUILD)/Makefile' 2>/dev/null)

_BUILD_LIBS := $(shell \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    '$(XTC_BUILD)/Makefile' 2>/dev/null)

CFLAGS  = -O3 -std=c11 -Wall -Wextra -Wpedantic \
          -I$(XTC_INC) \
          -I$(HIST_DIR)

LDFLAGS = $(XTC_BUILD)/libxtc.a $(_BUILD_LDFLAGS) $(_BUILD_LIBS)

.PHONY: all clean

all: bench

# hist.c provides the HIST_IMPLEMENTATION translation unit so main.c
# does not need to define HIST_IMPLEMENTATION itself.
bench: main.c $(HIST_DIR)/hist.c $(XTC_BUILD)/libxtc.a
	$(CC) $(CFLAGS) $(CPPFLAGS) \
	    -o bench \
	    main.c \
	    $(HIST_DIR)/hist.c \
	    $(LDFLAGS)

clean:
	rm -f bench
