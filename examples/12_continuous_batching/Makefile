# Build libxtc with --enable-sched-policy first.
XTC_BUILD ?= ../../build_unix
XTC_INC ?= ../../src/inc
CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I$(XTC_INC)
XTC_LDFLAGS := $(shell sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)
XTC_LIBS := $(shell sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)

.PHONY: all test clean
all: continuous_batching
continuous_batching: main.c $(XTC_INC)/xtc_batch.h $(XTC_INC)/xtc_sched.h $(XTC_BUILD)/libxtc.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -o $@ main.c $(XTC_BUILD)/libxtc.a $(LDFLAGS) $(XTC_LDFLAGS) $(XTC_LIBS) -pthread

test: continuous_batching
	./continuous_batching --self-test
	./continuous_batching --help >/dev/null
	@if ./continuous_batching --mode=invalid >/dev/null 2>&1; then exit 1; fi
	@if ./continuous_batching --policy=invalid >/dev/null 2>&1; then exit 1; fi

clean:
	rm -f continuous_batching
