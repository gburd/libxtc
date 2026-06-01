# sqlxtc naming policy: morphing SQLite into an xtc-native engine

The goal is for sqlxtc to read as an xtc-native project, not a SQLite
wrapper.  This note records how we get there, and -- equally important
-- what we deliberately do NOT rename, with reasons, so the boundary is
explicit.

## What is named in xtc conventions (our code)

Everything we write follows xtc conventions:

  * The application layer (`main.c`, `conn.c`, `db.c`, `quack.c`,
    `sql_parse.c`, `metrics.c`, `admin.c`) is 100% free of `sqlite3_`
    -- it speaks only the `sx_` engine facade (`engine.h`).
  * The from-scratch engine (`bufmgr.c`, `btnode.c`, `btree.c`,
    `mvcc.c`, `wal.c`, `xstore.c`) uses project naming throughout.
  * The single SQLite boundary is the `sx_` facade plus the five
    extension SEAMS (`vfs.c`, `pcache.c`, `mutex.c`, `mem.c`,
    `xstore.c`).  Those seams must name `sqlite3_*` types and
    functions because they IMPLEMENT SQLite's documented extension
    interfaces -- they are callbacks SQLite itself invokes
    (`sqlite3_module`, `sqlite3_vfs`, `sqlite3_mem_methods`, ...).
    Their own symbols are xtc-conventioned; only the unavoidable
    vendored-API calls remain.

## What we do NOT rename, and why

  * **The vendored `sqlite3.c` / `sqlite3.h` internals** (`Vdbe`,
    `Btree`, `Parse`, the ~thousands of internal symbols).  Renaming a
    250k-line vendored amalgamation is high-risk churn (string
    literals, the build, subtle cross-references) for no benefit:
    those symbols are encapsulated behind the seams, and AGENTS.md
    explicitly allows vendored source.  We are also *replacing* this
    engine (the MVCC + B-tree work), so the symbols are transient.

  * **SQL-visible and file-format identifiers**: `sqlite_master` /
    `sqlite_schema`, `sqlite_sequence`, `sqlite_stat1..4`, the
    `"SQLite format 3\000"` header magic, `sqlite_version()`, PRAGMA
    names.  These are part of SQL SEMANTICS and the on-disk FILE
    FORMAT.  Renaming them would break queries that reference the
    schema tables, break `ANALYZE`, and make database files
    unreadable.  They are not ours to rename.

  * **The `sqlite3_*` public API the seams call.**  These could be
    aliased to xtc names with a mapping header, but that is cosmetic
    indirection over the one boundary that, by design, talks to the
    vendored engine.  It is clearer to leave the boundary visible: a
    `sqlite3_` call in this tree means "into the vendored engine,"
    nothing else does.

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
