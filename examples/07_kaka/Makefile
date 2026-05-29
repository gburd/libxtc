#-
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# examples/07_kaka/Makefile -- builds kaka-broker.
#
# Targets:
#   make            build kaka-broker
#   make clean      remove objects + binary
#
# Override XTC_BUILD to point at a different libxtc build dir.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

# Pull liburing include + link flags from the parent build, the same
# way the other examples do, so this links whatever I/O backend the
# library was configured with.
LIBURING_INC := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^CFLAGS[[:space:]]*=/{print}' $(XTC_BUILD)/Makefile 2>/dev/null \
    | grep -o '\-I[^ ]*liburing[^ ]*' | head -1)

XTC_LDFLAGS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LDFLAGS[[:space:]]*=/{sub(/^LDFLAGS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

LIBS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

CFLAGS  += $(LIBURING_INC)
LDFLAGS  = -pthread $(XTC_BUILD)/libxtc.a $(XTC_LDFLAGS) $(LIBS)

SRCS    = main.c frame.c partition.c broker.c
OBJS    = $(SRCS:.c=.o)
TARGET  = kaka-broker

.PHONY: all clean test

all: $(TARGET) test_frame test_partition test_broker

test_frame: test_frame.c frame.c frame.h
	$(CC) $(CFLAGS) -o $@ test_frame.c frame.c

test_partition: test_partition.c partition.c frame.h partition.h
	$(CC) $(CFLAGS) -o $@ test_partition.c partition.c

test_broker: test_broker.c broker.c frame.c partition.c broker.h
	$(CC) $(CFLAGS) -o $@ test_broker.c broker.c frame.c partition.c $(LDFLAGS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) test_frame test_partition test_broker
