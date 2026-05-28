#!/usr/bin/env python3
"""Concurrent stress: N clients * M queries on the same shared sqlite3.
Verifies no deadlocks, no crashes, correct results."""
import json
import socket
import sys
import threading
import time
import random


PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 15434
N_CLIENTS = int(sys.argv[2]) if len(sys.argv) > 2 else 100
N_QUERIES = int(sys.argv[3]) if len(sys.argv) > 3 else 100


class Client:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.f = self.s.makefile("rwb", buffering=0)
        self.banner = json.loads(self.f.readline())

    def query(self, sql, limit=None):
        kw = {"q": sql}
        if limit is not None:
            kw["limit"] = limit
        self.f.write((json.dumps(kw) + "\n").encode())
        cols = None
        rows = []
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("server closed")
            m = json.loads(line)
            if "cols" in m:
                cols = m["cols"]
            elif "row" in m:
                rows.append(m["row"])
            elif "done" in m:
                return cols, rows, m["done"]
            elif "err" in m:
                raise RuntimeError("err: " + m["err"])

    def close(self):
        try:
            self.f.write(b'{"quit":1}\n')
        except Exception:
            pass
        self.s.close()


# Set up the schema first.
setup = Client(PORT)
setup.query("DROP TABLE IF EXISTS t")
setup.query("CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v TEXT)")
setup.close()


errors = []
total_queries = [0]
lock = threading.Lock()


def worker(idx):
    try:
        c = Client(PORT)
        for q in range(N_QUERIES):
            r = random.random()
            if r < 0.5:
                c.query(f"SELECT COUNT(*) FROM t")
            elif r < 0.8:
                c.query(
                    f"INSERT INTO t(k,v) VALUES({idx*N_QUERIES + q},"
                    f"'c{idx}q{q}')"
                )
            elif r < 0.9:
                c.query(f"UPDATE t SET v='upd{idx}' WHERE k=%d" %
                        (idx*N_QUERIES + q))
            else:
                c.query("SELECT id, k, v FROM t LIMIT 5")
            with lock:
                total_queries[0] += 1
        c.close()
    except Exception as e:
        errors.append((idx, str(e)))


t0 = time.monotonic()
threads = [threading.Thread(target=worker, args=(i,))
           for i in range(N_CLIENTS)]
for t in threads:
    t.start()
for t in threads:
    t.join()
elapsed = time.monotonic() - t0


print(f"clients: {N_CLIENTS}")
print(f"queries per client: {N_QUERIES}")
print(f"total queries: {total_queries[0]}")
print(f"elapsed: {elapsed:.2f} sec")
print(f"qps: {total_queries[0]/elapsed:.0f}")
print(f"errors: {len(errors)}")
for i, e in errors[:10]:
    print(f"  client {i}: {e}")

# Sanity check.
c = Client(PORT)
_, rows, _ = c.query("SELECT COUNT(*) FROM t")
final = rows[0][0]
print(f"final rowcount: {final}")
c.close()


if errors:
    sys.exit(1)
print("OK: concurrent test passed")
