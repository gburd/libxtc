#!/usr/bin/env python3
"""test/sqlxtc/test_sqlxtc_iosoak.py -- concurrent network + storage soak.

N clients for SECS seconds over TCP against one sqlxtc-server whose
durable store is larger than its buffer pool, so commits (WAL fsync),
page eviction and page reads from disk all run under the network load.
Each client owns a disjoint key range and keeps a local model of it:

  70% INSERT a 1-3.9 KiB row (sqlxtc's row limit is 4 KiB: XS_VMAX),
  15% UPDATE one of its rows to a new value,
  10% point SELECT (checked against the model), 5% range SELECT of up to
  50 of its rows (checked).

After the clients stop, every client's rows are read back through a new
connection and compared with its model (no lost, stale or extra row).
Prints one line: ops=N ops_per_s=X rows=R bytes=B errors=0
Exit 1 on any mismatch or error.
"""
import hashlib
import json
import random
import socket
import sys
import threading
import time

PORT = int(sys.argv[1])
CLIENTS = int(sys.argv[2])
SECS = float(sys.argv[3])
SPAN = 1 << 20          # keys per client


class Client:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=60)
        self.f = self.s.makefile("rwb", buffering=0)
        json.loads(self.f.readline())

    def q(self, sql, params=None):
        msg = {"q": sql}
        if params is not None:
            msg["params"] = params
        self.f.write((json.dumps(msg) + "\n").encode())
        rows = []
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("server closed")
            m = json.loads(line)
            if "row" in m:
                rows.append(m["row"])
            elif "done" in m:
                return rows
            elif "err" in m:
                raise RuntimeError("err: " + m["err"] + " in: " + sql[:60])

    def close(self):
        try:
            self.f.write(b'{"quit":1}\n')
        except OSError:
            pass
        self.s.close()


def value(k, gen, n):
    # deterministic, checkable payload: a digest stretched to n bytes
    h = hashlib.sha256(f"{k}:{gen}".encode()).hexdigest()
    return (h * (n // len(h) + 1))[:n]


errors = []
ops = [0]
lock = threading.Lock()
models = {}


def worker(idx):
    rnd = random.Random(idx)
    base = idx * SPAN
    model = {}            # k -> (gen, n)
    done = 0
    try:
        c = Client()
        end = time.monotonic() + SECS
        while time.monotonic() < end:
            r = rnd.random()
            if r < 0.70 or not model:
                k = base + len(model)
                n = rnd.randint(1024, 4000)
                c.q("INSERT INTO s(k, gen, v) VALUES(?1, 0, ?2)",
                    [k, value(k, 0, n)])
                model[k] = (0, n)
            elif r < 0.85:
                k = rnd.choice(list(model))
                gen, _ = model[k]
                n = rnd.randint(1024, 4000)
                c.q("UPDATE s SET gen = ?2, v = ?3 WHERE k = ?1",
                    [k, gen + 1, value(k, gen + 1, n)])
                model[k] = (gen + 1, n)
            elif r < 0.95:
                k = rnd.choice(list(model))
                rows = c.q("SELECT gen, v FROM s WHERE k = ?1", [k])
                gen, n = model[k]
                if rows != [[gen, value(k, gen, n)]]:
                    raise RuntimeError(f"point read k={k}: stale or wrong")
            else:
                lo = base + rnd.randrange(len(model))
                rows = c.q("SELECT k, gen, length(v) FROM s WHERE k >= ?1 "
                           "AND k < ?2 ORDER BY k LIMIT 50",
                           [lo, base + SPAN])
                want = [[k, model[k][0], model[k][1]]
                        for k in sorted(x for x in model if x >= lo)][:50]
                if rows != want:
                    raise RuntimeError(f"range read from {lo}: mismatch")
            done += 1
        c.close()
    except Exception as e:  # noqa: BLE001 -- report every failure
        with lock:
            errors.append(f"client {idx}: {e}")
    with lock:
        ops[0] += done
        models[idx] = model


setup = Client()
setup.q("DROP TABLE IF EXISTS s")
setup.q("CREATE TABLE s(k INTEGER PRIMARY KEY, gen INTEGER, v TEXT)")
setup.close()

t0 = time.monotonic()
threads = [threading.Thread(target=worker, args=(i,)) for i in range(CLIENTS)]
for t in threads:
    t.start()
for t in threads:
    t.join()
elapsed = time.monotonic() - t0

# Read everything back and compare with the models.
nrows = 0
nbytes = 0
if not errors:
    c = Client()
    for idx, model in sorted(models.items()):
        base = idx * SPAN
        rows = c.q("SELECT k, gen, v FROM s WHERE k >= ?1 AND k < ?2 "
                   "ORDER BY k", [base, base + SPAN])
        want = [[k, model[k][0], value(k, *model[k])] for k in sorted(model)]
        if rows != want:
            errors.append(f"client {idx}: final contents differ "
                          f"({len(rows)} rows read, {len(want)} expected)")
        nrows += len(want)
        nbytes += sum(n for _, n in model.values())
    c.close()

print(f"ops={ops[0]} ops_per_s={ops[0] / elapsed:.0f} rows={nrows} "
      f"bytes={nbytes} errors={len(errors)}")
for e in errors[:10]:
    print("FAIL:", e)
sys.exit(1 if errors else 0)
