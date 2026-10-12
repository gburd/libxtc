# Build libxtc with --enable-sched-policy first (see README).
# FAKE backend (default): links only libxtc.a + pthread, no OpenCL.
# OPENCL backend: make XTC_WITH_OPENCL=1 (links -lOpenCL).
XTC_BUILD ?= ../../.agent/final-native
XTC_INC ?= ../../src/inc
CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I$(XTC_INC)
XTC_LDFLAGS := $(shell sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)
XTC_LIBS := $(shell sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)

ifeq ($(XTC_WITH_OPENCL),1)
CPPFLAGS += -DXTC_WITH_OPENCL
OCL_LIBS = -lOpenCL
else
OCL_LIBS =
endif

.PHONY: all test clean
all: mandel
mandel: mandel.c $(XTC_INC)/xtc_batch.h $(XTC_INC)/xtc_future.h $(XTC_BUILD)/libxtc.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -o $@ \
	    mandel.c $(XTC_BUILD)/libxtc.a $(LDFLAGS) $(XTC_LDFLAGS) \
	    $(XTC_LIBS) $(OCL_LIBS) -pthread

test: mandel
	./mandel --self-test
	./mandel --help >/dev/null
	@if ./mandel --backend=bogus >/dev/null 2>&1; then exit 1; fi
	@if ./mandel --devices=9 >/dev/null 2>&1; then exit 1; fi

clean:
	rm -f mandel
