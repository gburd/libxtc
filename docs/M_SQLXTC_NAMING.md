# sqlxtc naming policy: morphing SQLite into an xtc-native engine

The goal is for sqlxtc to read as an xtc-native project, not a SQLite
wrapper.  This note records how we get there, and -- equally important
-- what we deliberately do NOT rename, with reasons, so the boundary is
explicit.

## What is named in xtc conventions (our code)

Everything we write follows xtc conventions, and `sqlite3_` no longer
appears in sqlxtc's own source:

  * The application layer (`main.c`, `conn.c`, `db.c`, `quack.c`,
    `sql_parse.c`, `metrics.c`, `admin.c`) speaks only the `sx_`
    engine facade (`engine.h`).
  * The from-scratch engine (`bufmgr.c`, `btnode.c`, `btree.c`,
    `mvcc.c`, `wal.c`, `xstore.c`) uses project naming throughout.
  * The five extension SEAMS (`vfs.c`, `pcache.c`, `mutex.c`, `mem.c`,
    `xstore.c`) implement SQLite's callback interfaces, so they must
    name the vendored engine's public API and types -- but they do so
    under project-native `xsql_*` names, not `sqlite3_*`.  The rename
    is mechanical and total over the API we use: `xsql.h` maps every
    `sqlite3_<name>` identifier this example references to `xsql_<name>`
    (and the bare `sqlite3` handle type to `xsql`).  It is
    FORCE-INCLUDED (`-include xsql.h`) into both the vendored
    `sqlite3.c` compile and the seam compiles, so a renamed definition
    and every reference resolve to the same `xsql_` symbol.  The
    `#define`s are token-precise, so they touch only the standalone
    `sqlite3` handle and the `sqlite3_<name>` public identifiers --
    never the vendored internals (`sqlite3VdbeExec`, ...) and never
    string literals.

## What we do NOT rename, and why

  * **The vendored `sqlite3.c` / `sqlite3.h` internals** (`Vdbe`,
    `Btree`, `Parse`, the camel-cased `sqlite3Foo` symbols).  Renaming
    a 250k-line vendored amalgamation is high-risk churn for no
    benefit: those symbols are encapsulated, and we are *replacing*
    this engine.  (The public `sqlite3_*` API IS renamed, but only at
    the seam boundary via the token map above -- the amalgamation body
    is untouched apart from that mechanical, force-included rename.)

  * **SQL-visible and file-format identifiers**: `sqlite_master` /
    `sqlite_schema`, `sqlite_sequence`, `sqlite_stat1..4`, the
    `"SQLite format 3\000"` header magic, `sqlite_version()`, PRAGMA
    names.  These are part of SQL SEMANTICS and the on-disk FILE
    FORMAT.  The `xsql.h` map is token-precise and never rewrites
    string literals, so these are untouched and SQL / on-disk
    compatibility is preserved.

  * **The `"sqlite3.h"` include and the `sqlite3.c` filename.**  The
    vendored file keeps its upstream name (it is a verbatim drop); the
    seams include it as `"sqlite3.h"` and the symbol map does the
    rest.

## The real morph is engine replacement

The genuine way SQLite becomes sqlxtc is not a rename -- it is
swapping SQLite's subsystems for the libxtc-native engine, after which
the vendored symbols simply leave the tree:

  * storage  -> `xstore` over `btree`/`bufmgr` (done; SQL runs on it)
  * MVCC     -> versioned `xstore` + the 2PC coordinator / HLC
                (`mvcc.c`), PostgreSQL-style visibility + Cahill SSI
                (`M_SQLXTC_MVCC_SQL.md`)
  * eventually the SQL front end (parser/planner/VDBE) -- the largest
    remaining piece, at which point `sqlite3.c` is retired and the
    last `sqlite3_*` references go with it.

So the naming convergence is gated on, and achieved by, the
replacement work -- not by editing a vendored blob we are on track to
delete.
