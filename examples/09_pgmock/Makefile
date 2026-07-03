#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/09_pgmock/Makefile -- builds pgmock-server.
#
#   make        build pgmock-server (mock PostgreSQL postmaster)
#   make clean  remove objects + binary
#
# Override XTC_BUILD to point at a different libxtc build dir.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

# Pull liburing include + link flags from the parent build (matches
# the 06_sqlxtc pattern; a no-op when the epoll backend is selected).
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

SRCS = main.c listener.c backend.c pg_latch.c
OBJS = $(SRCS:.c=.o)
TARGET = pgmock-server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: main.c listener.h
listener.o: listener.c listener.h backend.h
backend.o: backend.c backend.h pg_latch.h
pg_latch.o: pg_latch.c pg_latch.h

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf *.dSYM
