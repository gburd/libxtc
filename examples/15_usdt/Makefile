# Build libxtc first, with probes to trace (see README):
#   ../../dist/configure --enable-dtrace          (FreeBSD, illumos, macOS)
#   ../../dist/configure --enable-dtrace=sdt      (Linux)
# The root `make check` also builds and runs this as usdt_convoy.
XTC_BUILD ?= ../../build_unix
XTC_INC ?= ../../src/inc
CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I$(XTC_INC) -I$(XTC_BUILD)
XTC_LDFLAGS := $(shell sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)
XTC_LIBS := $(shell sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)

.PHONY: all test clean
all: convoy
convoy: convoy.c $(XTC_BUILD)/libxtc.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -o $@ \
	    convoy.c $(XTC_BUILD)/libxtc.a $(LDFLAGS) $(XTC_LDFLAGS) \
	    $(XTC_LIBS) -pthread

test: convoy
	./convoy convoy
	./convoy fixed
	./convoy lostwake
	./convoy --help >/dev/null

clean:
	rm -f convoy
