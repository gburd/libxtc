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

SRCS    = main.c frame.c partition.c broker.c group.c metrics.c
OBJS    = $(SRCS:.c=.o)
TARGET  = kaka-broker

TESTS   = test_frame test_partition test_broker test_group

.PHONY: all clean test

all: $(TARGET) $(TESTS) kaka_load

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_frame: test_frame.c frame.c frame.h
	$(CC) $(CFLAGS) -o $@ test_frame.c frame.c

test_partition: test_partition.c partition.c frame.c frame.h partition.h
	$(CC) $(CFLAGS) -o $@ test_partition.c partition.c frame.c $(LDFLAGS)

test_broker: test_broker.c broker.c frame.c partition.c metrics.c group.c broker.h
	$(CC) $(CFLAGS) -o $@ test_broker.c broker.c frame.c partition.c metrics.c group.c $(LDFLAGS)

test_group: test_group.c group.c group.h partition.c frame.c
	$(CC) $(CFLAGS) -o $@ test_group.c group.c partition.c frame.c $(LDFLAGS)

# Produce/consume load driver (a plain socket client, no libxtc).
kaka_load: kaka_load.c
	$(CC) -O2 -g -std=c11 -Wall -Wextra -o $@ kaka_load.c

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(TESTS) kaka_load
	rm -rf *.dSYM
