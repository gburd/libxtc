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
        return self.result()

    def result(self):
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

    # Row size limit: a row just under it stores and reads back; one over
    # it is refused with an error that names the limit (not "unsupported
    # SQL"), and the connection survives.
    c.query("CREATE TABLE big(id INTEGER PRIMARY KEY, v TEXT)")
    c.query("INSERT INTO big VALUES (1, '%s')" % ("a" * 4000))
    cols, rows, n = c.query("SELECT length(v) FROM big WHERE id = 1")
    check("row under limit", rows == [[4000]])
    for sql in ("INSERT INTO big VALUES (2, '%s')" % ("b" * 5000),
                "UPDATE big SET v = '%s' WHERE id = 1" % ("c" * 5000)):
        try:
            c.query(sql)
            check("row over limit refused", False)
        except RuntimeError as e:
            check("row over limit refused", "row too large" in str(e))
    cols, rows, n = c.query("SELECT id, length(v) FROM big ORDER BY id")
    check("row over limit left table intact", rows == [[1, 4000]])

    # Per-connection transactions (default --threads 1).  Each client owns
    # its own transaction: another client's uncommitted writes are
    # invisible, COMMIT/ROLLBACK affect only the issuing client, and an
    # autocommit statement from one client is never absorbed into another
    # client's open transaction.
    a, o = Client(PORT), Client(PORT)
    a.query("CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER)")
    a.query("INSERT INTO acct(id,bal) VALUES(1,100)")
    a.query("BEGIN")
    a.query("UPDATE acct SET bal=999 WHERE id=1")
    a.query("INSERT INTO acct(id,bal) VALUES(2,5)")
    _, rows, _ = o.query("SELECT bal FROM acct WHERE id=1")
    check("no dirty read of an open UPDATE", rows == [[100]])
    _, rows, _ = o.query("SELECT bal FROM acct WHERE id=2")
    check("no dirty read of an open INSERT", rows == [])
    o.query("INSERT INTO acct(id,bal) VALUES(3,7)")   # autocommit, other conn
    a.query("ROLLBACK")
    _, rows, _ = o.query("SELECT id, bal FROM acct ORDER BY id")
    check("ROLLBACK undoes only its own txn", rows == [[1, 100], [3, 7]])
    a.query("BEGIN")
    a.query("UPDATE acct SET bal=bal+10 WHERE id=1")
    o.query("BEGIN")
    o.query("UPDATE acct SET bal=bal+1 WHERE id=3")
    o.query("ROLLBACK")
    a.query("COMMIT")
    _, rows, _ = o.query("SELECT id, bal FROM acct ORDER BY id")
    check("txns are not merged across conns", rows == [[1, 110], [3, 7]])

    # Lost update (default SNAPSHOT): both read 110, both add.  The
    # second UPDATE waits for the first writer's row intent (so it is
    # only sent here; its reply comes once A ends) and, A having
    # committed, fails with a serialization error AT THE UPDATE
    # (first-updater-wins, as PostgreSQL).  The block then refuses every
    # statement until it is ended; COMMIT reports the failure.
    a.query("BEGIN")
    o.query("BEGIN")
    a.query("SELECT bal FROM acct WHERE id=1")
    o.query("SELECT bal FROM acct WHERE id=1")
    a.query("UPDATE acct SET bal=bal+10 WHERE id=1")
    o.send(q="UPDATE acct SET bal=bal+20 WHERE id=1")
    a.query("COMMIT")
    try:
        o.result()
        check("lost update is refused at the UPDATE", False)
    except RuntimeError as e:
        check("lost update is refused at the UPDATE", "serialize" in str(e))
    try:
        o.query("SELECT bal FROM acct WHERE id=1")
        check("aborted block refuses statements", False)
    except RuntimeError as e:
        check("aborted block refuses statements", "aborted" in str(e))
    try:
        o.query("COMMIT")
        check("COMMIT of an aborted block fails", False)
    except RuntimeError as e:
        check("COMMIT of an aborted block fails", "serialize" in str(e))
    _, rows, _ = o.query("SELECT bal FROM acct WHERE id=1")
    check("first updater wins", rows == [[120]])
    # A duplicate primary key is a UNIQUE violation, not "unsupported SQL".
    try:
        a.query("INSERT INTO acct(id,bal) VALUES(1,0)")
        check("duplicate key is refused", False)
    except RuntimeError as e:
        check("duplicate key is refused",
              "UNIQUE constraint failed: acct.id" in str(e))
    # SERIALIZABLE is selectable over the wire.
    a.query("BEGIN")
    a.query("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE")
    a.query("COMMIT")
    a.query("BEGIN ISOLATION LEVEL SERIALIZABLE")
    a.query("COMMIT")
    a.query("START TRANSACTION ISOLATION LEVEL REPEATABLE READ")
    a.query("COMMIT")
    a.query("SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL"
            " SNAPSHOT")
    check("isolation statements accepted", True)
    a.close()
    o.close()

    c.close()

    if fails:
        print(f"FAIL: {fails} checks failed")
        sys.exit(1)
    print("OK: all smoke checks passed")


if __name__ == "__main__":
    main()
