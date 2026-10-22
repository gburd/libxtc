# 15_usdt -- finding a lock convoy and a lost reply with USDT probes

`convoy.c` is a deliberately buggy program for the libxtc static probes
(`man xtc_usdt`, `docs/guide/12-tracing.md`).  It has three modes:

| mode       | what it does                                                         |
|------------|----------------------------------------------------------------------|
| `convoy`   | 8 fibers take one `xtc_amutex` and sleep 200 us *holding it*          |
| `fixed`    | the same work, with the sleep moved outside the lock                  |
| `lostwake` | a server forgets one reply in ten; the client waits out its timeout   |

Every mode exits 0: the bugs show up only as time, which is what makes them
a tracing problem.  `make check` builds and runs all three.

## Build

Configure libxtc with probes, then build the example (the root build
tree calls it `usdt_convoy`; this directory's Makefile calls it `convoy`):

```sh
mkdir build_unix && cd build_unix
../dist/configure --enable-dtrace        # FreeBSD, illumos, macOS
../dist/configure --enable-dtrace=sdt    # Linux (systemtap <sys/sdt.h>)
make && make usdt_convoy
# or: cd ../examples/15_usdt && make XTC_BUILD=../../build_unix
```

## The convoy

```sh
dtrace -qs ../tools/dtrace/xtc_lock_contention.d -c './usdt_convoy convoy'
dtrace -qs ../tools/dtrace/xtc_lock_contention.d -c './usdt_convoy fixed'
```

`convoy` reports one amutex with a contended wait on almost every
acquisition (159 of 160 on a FreeBSD 15.1 host), most of them 1-2 ms: seven
fibers queued ahead, each sleeping 200 us under the lock.  `fixed` reports
no contended waits, and the run drops from about 36 ms to about 4.5 ms.  The stack
aggregation at the end points at `worker` in `convoy.c`.

On Linux:

```sh
sudo bpftrace ../tools/bpftrace/xtc_lock_contention.bt -c './usdt_convoy convoy'
```

## The lost reply

```sh
dtrace -qs ../tools/dtrace/xtc_park_latency.d -c './usdt_convoy lostwake'
```

The mailbox park latency (reason 1) is bimodal: most parks end within
microseconds, seven end near 20 ms -- the client's timeout (four on the
client, three on the server waiting for its next request).  Each of those
is a park with no `fiber-wake` until the timer fires.  For a program that
waits forever instead of timing out, `xtc_lost_wake.d -p PID` names the
fiber, the reason and the object it is parked on.
