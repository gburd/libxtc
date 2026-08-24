#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# test/sqlxtc/compat_common.py
#
# Shared plumbing for the sqlxtc SQL-compatibility harnesses:
#   - Quack        JSON-over-TCP client for the sqlxtc server
#   - normalise_rows  makes Quack vs sqlite3 result sets comparable
#   - split_statements  splits a ';'-separated SQL batch (quote/comment aware)
#   - Server       spawns/reuses one sqlxtc-server process, one fresh DB
#                  file per fresh() call (a new connection == a new schema)
#
# Extracted from test_sqlxtc_oracle.py so both the wired-in CI test and
# the opt-in ~/src/sqlite harvester share one implementation.

import json
import os
import socket
import subprocess
import time


class Quack:
    def __init__(self, port, host="127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=15)
        self.f = self.s.makefile("rwb", buffering=0)
        self.f.readline()          # skip hello banner

    def query(self, sql):
        self.f.write((json.dumps({"q": sql}) + "\n").encode())
        cols = None
        rows = []
        while True:
            line = self.f.readline()
            if not line:
                return cols, rows, "(connection closed)"
            o = json.loads(line.decode().strip())
            if "cols" in o:
                cols = o["cols"]
            elif "row" in o:
                rows.append(tuple(o["row"]))
            elif "done" in o:
                return cols, rows, None
            elif "err" in o:
                return cols, rows, o["err"]

    def close(self):
        try:
            self.f.write(b'{"quit":1}\n')
        except Exception:
            pass
        try:
            self.s.close()
        except Exception:
            pass


def normalise_rows(rows):
    """Make Quack vs sqlite3 row sets comparable and order-insensitive.

    SQLite returns bytes for BLOB; Quack returns a shaped string.
    Floats may differ in formatting; NULLs are tagged; ints that equal
    a float value are still distinguished by tag so 1 != 1.0 stays a
    real difference only when the tags differ -- but SQLite type
    affinity means we tag numerically to avoid spurious int/float
    splits on values SQLite itself treats as equal in a result set."""
    out = []
    for r in rows:
        new = []
        for v in r:
            if isinstance(v, bool):
                new.append(("i", int(v)))
            elif isinstance(v, float):
                # Integral floats compare equal to ints in SQL results.
                if v == int(v):
                    new.append(("num", float(v)))
                else:
                    new.append(("num", round(v, 6)))
            elif isinstance(v, int):
                new.append(("num", float(v)))
            elif v is None:
                new.append(("n", 0))
            elif isinstance(v, bytes):
                new.append(("s", v.decode("ascii", errors="replace")))
            elif isinstance(v, str):
                new.append(("s", v))
            else:
                new.append(("o", repr(v)))
        out.append(tuple(new))
    return sorted(out, key=repr)


def split_statements(batch):
    """Split a ';'-separated SQL batch into statements, respecting
    single/double quotes, [bracket] and `backtick` identifiers, and
    -- line / /* block */ comments.  Returns non-empty statements."""
    stmts = []
    buf = []
    i = 0
    n = len(batch)
    while i < n:
        c = batch[i]
        if c == "'" or c == '"' or c == '`':
            q = c
            buf.append(c)
            i += 1
            while i < n:
                buf.append(batch[i])
                if batch[i] == q:
                    # doubled quote == escaped, stay in string
                    if i + 1 < n and batch[i + 1] == q:
                        buf.append(batch[i + 1])
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            continue
        if c == "[":
            buf.append(c)
            i += 1
            while i < n:
                buf.append(batch[i])
                if batch[i] == "]":
                    i += 1
                    break
                i += 1
            continue
        if c == "-" and i + 1 < n and batch[i + 1] == "-":
            while i < n and batch[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and batch[i + 1] == "*":
            i += 2
            while i + 1 < n and not (batch[i] == "*" and batch[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == ";":
            s = "".join(buf).strip()
            if s:
                stmts.append(s)
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    tail = "".join(buf).strip()
    if tail:
        stmts.append(tail)
    return stmts


class Server:
    """One sqlxtc-server process.  fresh() returns a brand-new Quack
    connection backed by a fresh DB file, giving each caller an empty
    schema without paying a full process restart every time."""

    def __init__(self, port=16455, server_bin=None):
        here = os.path.abspath(os.path.dirname(__file__))
        self.bin = server_bin or os.path.normpath(os.path.join(
            here, "..", "..", "examples", "06_sqlxtc", "sqlxtc-server"))
        self.port = port
        self.proc = None
        self.log = None
        self._n = 0
        self._cur = None

    def available(self):
        return os.access(self.bin, os.X_OK)

    def _spawn(self, db):
        subprocess.call(
            ["pkill", "-9", "-f", "sqlxtc-server.*-p %d" % self.port],
            stderr=subprocess.DEVNULL)
        time.sleep(0.05)
        if os.path.exists(db):
            os.unlink(db)
        self.log = open("/tmp/sqlxtc-compat.log", "ab")
        self.proc = subprocess.Popen(
            [self.bin, "-p", str(self.port), "-d", db],
            stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log,
            start_new_session=True)
        # wait for listener
        for _ in range(60):
            try:
                q = Quack(self.port)
                q.close()
                return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("sqlxtc-server failed to come up")

    def start(self):
        self._db = "/tmp/sqlxtc-compat-%d.db" % os.getpid()
        self._spawn(self._db)

    def fresh(self):
        """New empty schema.  We restart the server on a fresh DB file;
        cheap enough (~0.3s) and guarantees isolation between test files."""
        if self._cur is not None:
            self._cur.close()
        self._n += 1
        db = "/tmp/sqlxtc-compat-%d-%d.db" % (os.getpid(), self._n)
        # kill previous and relaunch on the new file
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        self._spawn(db)
        # clean up the prior db file
        prev = "/tmp/sqlxtc-compat-%d-%d.db" % (os.getpid(), self._n - 1)
        for p in (prev, prev + "-wal", prev + "-shm"):
            if os.path.exists(p):
                try:
                    os.unlink(p)
                except OSError:
                    pass
        self._cur = Quack(self.port)
        return self._cur

    def stop(self):
        if self._cur is not None:
            self._cur.close()
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        for suf in ("", "-wal", "-shm"):
            for k in range(self._n + 1):
                p = "/tmp/sqlxtc-compat-%d-%d.db%s" % (os.getpid(), k, suf)
                if os.path.exists(p):
                    try:
                        os.unlink(p)
                    except OSError:
                        pass
        if self.log:
            self.log.close()
