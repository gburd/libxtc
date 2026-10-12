# Build libxtc with --enable-sched-policy first (see README).
# FAKE backend (default): links only libxtc.a + pthread, no OpenCL/Level Zero.
# GPU (OpenCL) backend: make XTC_WITH_OPENCL=1   (links -lOpenCL)
# NPU (Level Zero) backend: make XTC_WITH_LEVELZERO=1  (links -lze_loader)
# The --hetero capstone needs BOTH: make XTC_WITH_OPENCL=1 XTC_WITH_LEVELZERO=1
XTC_BUILD ?= ../../.agent/final-native
XTC_INC ?= ../../src/inc
CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I$(XTC_INC)
XTC_LDFLAGS := $(shell sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)
XTC_LIBS := $(shell sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)

ACCEL_LIBS =

ifeq ($(XTC_WITH_OPENCL),1)
CPPFLAGS += -DXTC_WITH_OPENCL
ACCEL_LIBS += -lOpenCL
endif

ifeq ($(XTC_WITH_LEVELZERO),1)
CPPFLAGS += -DXTC_WITH_LEVELZERO
# Level Zero headers are in the main (non-split) output; pass ZE_INC to
# point at <prefix>/include when building inside the nix-shell.
ifneq ($(ZE_INC),)
CPPFLAGS += -I$(ZE_INC)
endif
ifneq ($(ZE_LIB),)
ACCEL_LIBS += -L$(ZE_LIB)
endif
ACCEL_LIBS += -lze_loader
endif

.PHONY: all test clean
all: hetero
hetero: hetero.c $(XTC_INC)/xtc_batch.h $(XTC_INC)/xtc_future.h $(XTC_BUILD)/libxtc.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -o $@ \
	    hetero.c $(XTC_BUILD)/libxtc.a $(LDFLAGS) $(XTC_LDFLAGS) \
	    $(XTC_LIBS) $(ACCEL_LIBS) -pthread

test: hetero
	./hetero --self-test
	./hetero --help >/dev/null
	@if ./hetero --reqs=1 >/dev/null 2>&1; then exit 1; fi
	@if ./hetero --reqs=9999 >/dev/null 2>&1; then exit 1; fi

clean:
	rm -f hetero
