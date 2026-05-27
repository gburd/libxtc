# bench/audit/Makefile
# Micro-benchmarks for the performance audit.
#
# Build the library first:
#   cd ../../build_unix && make
#
# Then build these benchmarks:
#   make

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -pthread
INCLUDES  = -I../../src/inc -I../../build_unix
XTC_LIB   = ../../build_unix/libxtc.a

# OpenSSL flags - adjust for your system.
# For Nix builds, set these environment variables or pass on command line.
SSL_LDFLAGS ?= -L/nix/store/by19gg2pqp2b1gqphdf6bwk2g3hgak79-openssl-3.6.2/lib -pthread
SSL_LIBS    ?= -lssl -lcrypto

PROGS = false_sharing_mpsc false_sharing_proc

all: $(PROGS)

false_sharing_mpsc: false_sharing_mpsc.c $(XTC_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(XTC_LIB) $(SSL_LDFLAGS) $(SSL_LIBS)

false_sharing_proc: false_sharing_proc.c $(XTC_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(XTC_LIB) $(SSL_LDFLAGS) $(SSL_LIBS)

run: $(PROGS)
	@echo "=== MPSC channel false-sharing benchmark ==="
	./false_sharing_mpsc
	@echo ""
	@echo "=== Proc mailbox false-sharing benchmark ==="
	./false_sharing_proc

clean:
	rm -f $(PROGS)

.PHONY: all run clean
