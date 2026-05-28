#!/usr/bin/env python3
"""End-to-end Quack protocol smoke test."""
import json
import socket
import sys


PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 15433


class Client:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.f = self.s.makefile("rwb", buffering=0)
        # Read banner.
        self.banner = json.loads(self.f.readline())

    def send(self, **obj):
        self.f.write((json.dumps(obj) + "\n").encode())

    def recv(self):
        line = self.f.readline()
        if not line:
            return None
        return json.loads(line)

    def query(self, sql, limit=None):
        kw = {"q": sql}
        if limit is not None:
            kw["limit"] = limit
        self.send(**kw)
        cols = None
        rows = []
        while True:
            m = self.recv()
            if m is None:
                raise RuntimeError("server closed")
            if "cols" in m:
                cols = m["cols"]
            elif "row" in m:
                rows.append(m["row"])
            elif "done" in m:
                return cols, rows, m["done"]
            elif "err" in m:
                raise RuntimeError("err: " + m["err"])
            else:
                raise RuntimeError("unexpected: " + repr(m))

    def close(self):
        try:
            self.send(quit=1)
        except Exception:
            pass
        self.s.close()


def main():
    fails = 0

    def check(name, cond):
        nonlocal fails
        if cond:
            print(f"  OK  {name}")
        else:
            print(f"  FAIL {name}")
            fails += 1

    c = Client(PORT)
    check("banner", c.banner.get("hello") == "sqlxtc")
    check("quack flag", c.banner.get("quack") == 1)

    # SELECT 1+1
    cols, rows, n = c.query("SELECT 1+1")
    check("SELECT 1+1 cols", cols == ["1+1"])
    check("SELECT 1+1 rows", rows == [[2]])
    check("SELECT 1+1 done", n == 1)

    # Ping
    c.send(ping=1)
    pong = c.recv()
    check("ping/pong", pong == {"pong": 1})

    # CREATE / INSERT / SELECT
    cols, rows, n = c.query("CREATE TABLE t(id INTEGER, name TEXT)")
    check("CREATE done", cols is None and rows == [])

    cols, rows, n = c.query("INSERT INTO t VALUES(1,'alice')")
    check("INSERT 1", n == 1)

    cols, rows, n = c.query("INSERT INTO t VALUES(2, NULL)")
    check("INSERT 2", n == 1)

    cols, rows, n = c.query("INSERT INTO t VALUES(3, 'bob')")
    check("INSERT 3", n == 1)

    cols, rows, n = c.query("SELECT * FROM t ORDER BY id")
    check("SELECT cols", cols == ["id", "name"])
    check("SELECT rows", rows == [[1, "alice"], [2, None], [3, "bob"]])
    check("SELECT done", n == 3)

    # LIMIT clause via JSON limit
    cols, rows, n = c.query("SELECT * FROM t ORDER BY id", limit=2)
    check("limit cap", n == 2 and len(rows) == 2)

    # UPDATE
    cols, rows, n = c.query("UPDATE t SET name='ALICE' WHERE id=1")
    check("UPDATE", n == 1)
    cols, rows, n = c.query("SELECT name FROM t WHERE id=1")
    check("UPDATE applied", rows == [["ALICE"]])

    # DELETE
    cols, rows, n = c.query("DELETE FROM t WHERE id=2")
    check("DELETE", n == 1)
    cols, rows, n = c.query("SELECT COUNT(*) FROM t")
    check("after delete count", rows == [[2]])

    # Error path
    try:
        c.query("SELECT * FROM nonexistent")
        check("syntax error path", False)
    except RuntimeError as e:
        check("syntax error path", "no such table" in str(e))

    # Connection survives error
    cols, rows, n = c.query("SELECT 42")
    check("survive after err", rows == [[42]])

    c.close()

    if fails:
        print(f"FAIL: {fails} checks failed")
        sys.exit(1)
    print("OK: all smoke checks passed")


if __name__ == "__main__":
    main()
