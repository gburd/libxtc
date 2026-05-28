#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/rexis_compat/Makefile

CC      = cc
CFLAGS  = -O3 -std=c11 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS = -lpthread

.PHONY: all clean

all: bench

bench: bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f bench
