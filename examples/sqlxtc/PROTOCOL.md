# Quack wire protocol

Quack is a deliberately tiny line-oriented JSON protocol over TCP for
sqlxtc.  It is named in homage to DuckDB but is **not** interoperable
with it.  The intent: easy to parse from any language, easy to debug
with `nc` and `cat`.

Each message is a single JSON object terminated by `\n`.  Messages are
not nested.  Servers must close the connection on framing errors that
they cannot recover from.

## Connection bringup

When a client connects, the server immediately sends:

    {"hello":"sqlxtc","version":"0.1","quack":1}\n

The client may pipeline before reading the banner.

## Client to server

Exactly one of these JSON keys is recognised per line:

    {"q":"SELECT 1+1"}
    {"q":"SELECT * FROM t","limit":1000}
    {"ping":1}
    {"quit":1}

* `q`        SQL string.  No multi-statement; the first statement wins.
* `limit`    Optional row cap; defaults to no limit.  Server-side limit
             applied after the SQL `LIMIT` clause if any.
* `ping`     Heartbeat; replies with `pong`.
* `quit`     Graceful shutdown; the server closes the socket without
             a reply.

Unrecognised top-level keys produce `{"err":"unknown message"}\n`.

## Server to client

For a `q` message the server emits, in order:

    {"cols":["c0","c1",...]}\n     # always present if rows follow
    {"row":[v0,v1,...]}\n          # zero or more
    {"row":[v0,v1,...]}\n
    {"done":N}\n                   # row count (or affected count for DML)

If the SQL produces no result set (DDL, DML without RETURNING) the
server omits `cols`/`row` and emits only:

    {"done":N}\n

On error the server emits:

    {"err":"<message>"}\n

and does not emit a `done`.

Pings reply:

    {"pong":1}\n

## JSON value mapping

SQLite type     -> JSON value
INTEGER         -> number (int64)
FLOAT           -> number (double; NaN/Inf rendered as null)
TEXT            -> string (UTF-8; control chars escaped as `\uXXXX`)
BLOB            -> string (base64 encoded; the `cols` description says
                  nothing about that, you have to know)
NULL            -> null

## Limits

* Max line length on input: 1 MiB by default (configurable).
* Max columns per row: 1024.
* Max rows per response: bounded by `limit` if given, otherwise by the
  client's read rate (we backpressure the server's write buffer).

## Errors

On a JSON parse failure the server emits

    {"err":"json: <reason>"}\n

and continues reading.  The connection is not closed.

On a SQL error the server emits

    {"err":"sql: <reason>"}\n

The connection is not closed; the next `q` is processed.

On exceeding `--max-iops` the server emits

    {"err":"OVER_LIMIT"}\n

On exceeding `--max-memory` the server emits

    {"err":"OOM"}\n

and closes the connection.

## Why JSON?

A real production wire protocol would be binary, length-prefixed, and
versioned.  We use newline JSON for two reasons: (1) it is trivial to
test from `nc` and python; (2) the example is about xtc, not about the
n-th wire protocol.  See `docs/M_LIBXTC_PG_BOUNDARY.md` for why a real
server would put the wire protocol on the application side of the
boundary.
