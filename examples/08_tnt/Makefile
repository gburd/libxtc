#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/08_tnt/Makefile -- builds the tnt Isolate layer + demos.
#
# Targets:
#   make            build libtnt.a + the echo server + the in-process test
#   make test       build and run the in-process dispatch test
#   make echo-test  build and run the TCP echo round-trip test
#   make clean      remove objects + binaries
#
# Override XTC_BUILD to point at a different libxtc build dir.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

# Pull liburing include flags from the parent build.
LIBURING_INC := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^CFLAGS[[:space:]]*=/{print}' $(XTC_BUILD)/Makefile 2>/dev/null \
    | grep -o '\-I[^ ]*liburing[^ ]*' \
    | head -1)

LIBURING_LDFLAGS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

XTC_LDFLAGS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LDFLAGS[[:space:]]*=/{sub(/^LDFLAGS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

CFLAGS  += $(LIBURING_INC)

LDFLAGS  = $(XTC_LDFLAGS) $(XTC_BUILD)/libxtc.a -pthread -ldl -lm \
           $(LIBURING_LDFLAGS)

TNT_OBJ = tnt.o

.PHONY: all clean test echo-test

all: libtnt.a tnt_echo test_tnt

libtnt.a: $(TNT_OBJ)
	ar rcs $@ $(TNT_OBJ)

%.o: %.c tnt.h
	$(CC) $(CFLAGS) -c -o $@ $<

# The canonical TCP echo server.
tnt_echo: echo.o libtnt.a
	$(CC) $(CFLAGS) -o $@ echo.o libtnt.a $(LDFLAGS)

# The in-process dispatch test (no network).
test_tnt: test_tnt.o libtnt.a
	$(CC) $(CFLAGS) -o $@ test_tnt.o libtnt.a $(LDFLAGS)

test: test_tnt
	./test_tnt

# End-to-end TCP echo round-trip test (spawns the server, connects,
# verifies echo).  Driven by a shell harness.
echo-test: tnt_echo
	bash ./test_echo.sh

clean:
	rm -f tnt.o echo.o test_tnt.o
	rm -f libtnt.a tnt_echo test_tnt
