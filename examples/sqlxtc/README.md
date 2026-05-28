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
cd ../examples/sqlxtc

# build sqlxtc-server (sqlite3.o is built once; ~30 sec)
make
```

The first make compiles the bundled SQLite amalgamation (`sqlite/sqlite3.c`,
9 MB, 250 K LOC).  Later builds reuse the object file.

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
                                  xtc_lwlock-backed mutex methods
                                  (xtc_mutex.c)
```

* `main.c` -- arg parsing, app/supervisor bringup, listener spawn.
* `conn.c` -- per-connection xtc_proc; reads lines, dispatches.
* `quack.c` -- hand-rolled JSON encoder/decoder for our small protocol.
* `db.c` -- sqlite3 handle management; result streaming via Quack.
* `xtc_mutex.c` -- sqlite3_mutex_methods backed by xtc_lwlock.
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
make test                              # unit tests (25 cases) + smoke
bash ../../test/sqlxtc/test_sqlxtc_smoke.sh        # end-to-end JSON
bash ../../test/sqlxtc/test_sqlxtc_concurrent.sh   # 100 clients (Phase 3)
bash ../../test/sqlxtc/test_sqlxtc_budgets.sh      # cap enforcement (Phase 4)
bash ../../bench/sqlxtc/saturate.sh 200 1000       # saturation bench
```

## Phases delivered

* Phase 0 -- scaffolding, vendored SQLite amalgamation, Lime submodule.
* Phase 1 -- Quack parser/encoder, per-conn xtc_proc, db_exec round-trip.
* Phase 2 -- Lime-generated SQL pre-parser; round-trip canonicalisation.
* Phase 3 -- xtc_lwlock-backed sqlite3_mutex_methods + serialized mode.
* Phase 4 -- resource budgets; saturation bench.
* Phase 5 -- libxtc / PostgreSQL boundary doc (`docs/M_LIBXTC_PG_BOUNDARY.md`).

## Performance

See `../../bench/sqlxtc/RESULTS.md`.  TL;DR on a 4-core run with
`--cores=4 --max-memory=1GiB --max-clients=200`:

* All 4 pinned cores reach ~100% CPU under the load.
* RSS plateaus below the configured cap.
* Reads (SELECT) saturate the SQLite serialised mutex; writes throttle
  through the shared lock.

## Gaps in xtc surfaced

Implementing sqlxtc surfaced several missing primitives we worked
around but should ideally exist:

1. **`xtc_proc_wait_fd`** -- the listener and per-connection procs
   busy-poll with `xtc_recv` 50 ms timeouts because xtc_proc cannot
   suspend on fd readiness.  This is the single biggest missing piece;
   it costs ~10-20 wakeups/sec/conn at idle.

2. **Recursive xtc_lwlock** -- `xMutexAlloc(SQLITE_MUTEX_RECURSIVE)`
   needs counting on top of xtc_lwlock.  We wrap it with an
   owner-tid + counter.  A first-class recursive mode in xtc_lwlock
   (or `xtc_amutex`) would simplify this.

3. **SQLite VFS over xtc_io** -- a full `sqlite3_vfs` that does async
   reads/writes via xtc_io would let SQLite never block the loop on
   disk I/O.  Not implemented; SQLite uses the default unix VFS.

4. **`SQLITE_CONFIG_MALLOC` integration** -- SQLite has a hookable
   allocator (`sqlite3_mem_methods`) that we could route through
   xtc_res so every SQLite allocation is charged to the budget.  Not
   implemented; we charge only sqlxtc's own allocations today, leaving
   the SQLite arena off-budget.

5. **ATTACH DATABASE per-DB resource scoping** -- `xtc_res_t` is a
   single global accountant.  A per-attached-DB budget would let
   operators give different SLAs to different tenants.

6. **Per-connection prepared statement cache** -- we re-prepare every
   SQL on every call.  A cache (LRU on the SQL text) would amortise
   parse+plan cost.  Belongs on the sqlxtc side, not in libxtc.

## License

ISC, like the rest of libxtc.  The bundled SQLite amalgamation is
public domain.  The bundled Lime parser generator is BSD-2-Clause
(see `lime/LICENSE`).
