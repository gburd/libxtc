# tools/sim-monitor/Makefile
#
# Standalone, optional -- NOT part of the library build, NOT invoked by
# make check, NOT referenced from dist/Makefile.in or meson.build.  See
# README.md.

CC      ?= cc
PKGCONF ?= pkg-config
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
XTC_INC ?= ../../src/inc

RAYLIB_CFLAGS := $(shell $(PKGCONF) --cflags raylib)
RAYLIB_LIBS   := $(shell $(PKGCONF) --libs raylib)

.PHONY: all clean
all: viewer

# recorder needs a libxtc.a built with --with-io-backend=sim; point
# XTC_SIM_LIB at it (see README.md step 1).
XTC_SIM_LIB ?= /tmp/xtcsim/libxtc.a

recorder: recorder.c
	$(CC) $(CFLAGS) -I$(XTC_INC) -o $@ $< $(XTC_SIM_LIB) -lm -pthread

viewer: viewer.c
	$(CC) $(CFLAGS) -o $@ $< $(RAYLIB_CFLAGS) $(RAYLIB_LIBS) -lm

clean:
	rm -f viewer recorder
