# sqlxtc -- networked, threaded SQLite via xtc

`sqlxtc` is a worked example that makes SQLite the back end of a multi-
client TCP server using the libxtc runtime end to end:

* `xtc_app` + supervisor for orchestration
* `xtc_proc` per connection
* `xtc_lwlock` as SQLite's mutex implementation (real lock, not pthread)
* `xtc_lrlock` for read-mostly catalog state
* `xtc_res` for hard caps on memory, FDs, in-flight tasks
* `xtc_net` for TCP listen/accept with reuseport
* `xtc_log` for structured logs

The wire protocol is **Quack** (PROTOCOL.md), a deliberately tiny line-
delimited JSON over TCP.  It is named in homage to DuckDB but is not
interoperable with DuckDB.

## Build

```sh
# build libxtc
cd ../..
mkdir -p build_unix && cd build_unix && ../dist/configure && make
cd ../examples/06_sqlxtc

# build sqlxtc-server (sqlite3.o is built once; ~30 sec)
make
```

The first make compiles the bundled SQLite amalgamation (`sqlite/sqlite3.c`,
9 MB, 250 K LOC).  Later builds reuse the object file.

To build sqlxtc against the single-file xtc amalgamation instead of
`libxtc.a` -- which exercises the amalgamation as a real, broad
consumer -- run `make amalg`.  It generates `amalg/xtc.c` + `xtc.h`,
compiles the xtc object, and links `sqlxtc-server-amalg` against it
(no libxtc.a, no liburing: the amalgamation auto-selects the epoll
backend).  The sqlxtc sources are compiled `-Iamalg/include`, where
forwarding stub headers resolve every `#include "xtc_*.h"` to the
single `xtc.h`.

## Run

```sh
# In-memory DB on the default port 15432.
nohup setsid ./sqlxtc-server -p 15432 -d :memory: \
    < /dev/null > /tmp/sqlxtc.log 2>&1 &
disown

# Talk to it from python:
python3 -c '
import socket, json
s = socket.create_connection(("127.0.0.1", 15432), timeout=5)
f = s.makefile("rwb", buffering=0)
print(f.readline().decode().strip())                      # hello banner
f.write(json.dumps({"q":"SELECT 1+1"}).encode() + b"\n")
for _ in range(3):
    print(f.readline().decode().strip())
'
```

## Architecture

```
              accept()                        spawn
listen_fd ----------> listener_proc ----xtc_proc-----> conn_proc(fd)
                          (xtc_proc)                       |
                                                           v
                                              read line, quack_parse,
                                              sql_parse pre-validate,
                                              db_exec via sqlite3,
                                              quack_emit row stream
                                                           |
                                                           v
                                                 send() to client

                                    +-----------------+
                                    |  shared sqlite3 |
                                    |  serialised mode|
                                    +-----------------+
                                              ^
                                  xtc_amutex-backed mutex methods
                                  (sqlxtc_mutex.c)
```

* `main.c` -- arg parsing, app/supervisor bringup, listener spawn.
* `conn.c` -- per-connection xtc_proc; reads lines, dispatches.
* `quack.c` -- hand-rolled JSON encoder/decoder for our small protocol.
* `db.c` -- sqlite3 handle management; result streaming via Quack.
* `sqlxtc_mutex.c` -- sqlite3_mutex_methods backed by xtc_amutex (the
  parking mutex).  Many connection processes share one serialized
  handle on a single loop, so a contender must PARK (yield the loop)
  rather than block the OS thread -- otherwise a backend that parks
  mid-statement (the VFS offload below) would wedge every peer.
  Recursion is tracked by FIBER identity (the proc), not thread id.
* `sqlxtc_vfs.c` -- a `"sqlxtc"` sqlite3_vfs (shim over the platform
  default).  Every byte of database I/O flows through it: per-file
  state is allocated with the xtc allocator, and reads, writes, and
  syncs are counted and timed with xtc_stats (the `sqlxtc.vfs.*`
  counters and latency histograms appear on the metrics line).
  Reads, writes, and fsyncs are offloaded via `xtc_blocking_run`, so
  the reactor thread never stalls on disk: the calling process parks
  while a pool thread does the syscall and the loop keeps serving
  peers (off a loop it falls back to a synchronous call).  Path
  operations and the byte-range file locks delegate to the base VFS so
  locking stays POSIX-correct.
* `sqlxtc_pcache.c` -- an xtc_slab-backed sqlite3_pcache_methods2.  Every
  page in one SQLite cache is the same size, so a per-cache xtc_slab
  supplies the page bodies with no fragmentation and O(1) alloc/free;
  a chained hash table indexes resident pages and an LRU list of
  unpinned pages feeds recycling, so the resident set stays bounded
  by SQLite's cache_size even when the working set is far larger.  Hit/miss/recycle and live-page
  counts go to xtc_stats (`sqlxtc.pcache.*`, also on the metrics
  line).  Both the VFS (I/O seam) and the pcache (memory seam) are
  supported SQLite extension points, so they route SQLite's internals
  through xtc primitives without forking SQLite.
* `sql_parse.c` -- pre-parser; classifies kind + readonly.  Phase 2 wires
  in a Lime-generated AST parser via `sql_parse_gen.c`.
* `sql_parse.lime` -- Lime grammar for the SQL subset we accept.
* `metrics.c` -- periodic xtc_res snapshot logger.

## Limits and flags

```
sqlxtc-server [options]
  -p, --port=PORT          bind port (default 15432)
  -d, --db=PATH            sqlite file (default :memory:)
  -c, --cores=N            pin to N cores via sched_setaffinity
  -m, --max-memory=BYTES   xtc_res memory cap; OOM error on exceed
  -n, --max-clients=N      reject new conns at the cap (default 1000)
  -i, --max-iops=N         queries/sec token bucket (0 = unlimited)
  -D, --max-databases=N    cap on ATTACHed dbs (default 16)
      --no-shared          per-connection sqlite3 (Phase 1 mode)
```

## Testing

```sh
make test                              # unit tests + in-process VFS+pcache tests + smoke
make test-vfs                          # xtc VFS test against libxtc.a
make test-vfs-amalg                    # xtc VFS test against the amalgamation
make test-pcache                       # xtc slab page-cache test
make test-pcache-amalg                 # ... against the amalgamation
make amalg                             # build sqlxtc against the single-file xtc
bash ../../test/sqlxtc/test_sqlxtc_smoke.sh        # end-to-end JSON
bash ../../test/sqlxtc/test_sqlxtc_concurrent.sh   # 100 clients (Phase 3)
bash ../../test/sqlxtc/test_sqlxtc_budgets.sh      # cap enforcement (Phase 4)
bash ../../bench/sqlxtc/saturate.sh 200 1000       # saturation bench
```

## Performance

See `../../bench/sqlxtc/RESULTS.md` for raw numbers.  Current state
is single-loop, single-sqlite3-handle, SQLITE_CONFIG_SERIALIZED.
Under concurrent client load:

* The xtc loop and the listener / connection procs distribute work
  across fibers cleanly; idle CPU is 0%.
* All connections speak to one shared sqlite3 handle through xtc_lwlock
  serving as SQLite's mutex layer.  This means SQL execution itself
  serializes -- the design demonstrates the wire protocol and the xtc
  primitive integration but does not yet scale across cores.
* Driving the existing saturation bench yields ~3000 qps for mixed
  SELECT/INSERT workloads on a memory-resident DB with one CPU core
  fully utilised.

Real multi-core scaling requires breaking SQLite's monolithic mutex
into fine-grained per-page / per-table locks.  That work is planned
in five phases (subsystem-as-server, then xtc_lrlock buffer pool,
then fine-grained btree locks, then async VFS) -- documented in
[`../../docs/M_SQLXTC_HARDFORK.md`](../../docs/M_SQLXTC_HARDFORK.md).
Until that lands, sqlxtc is a single-core SQLite server with an xtc-
shaped network frontend.  The eventual hard-fork is the project's
payoff.

## License

ISC, like the rest of libxtc.  The bundled SQLite amalgamation is
public domain.  The bundled Lime parser generator is BSD-2-Clause
(see `lime/LICENSE`).
