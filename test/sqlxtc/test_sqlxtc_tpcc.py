#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# test/sqlxtc/test_sqlxtc_tpcc.py
#
# Differential test of the TPROC-C statement surface, against python's
# sqlite3 as the oracle.  Two drivers are covered:
#
#   * the Turso C harness (perf/tpc-c): its 35 prepared statements,
#     sent with bound ? parameters over the Quack "params" field, on
#     Turso's own schema (composite PRIMARY KEYs, the 4 CREATE INDEX);
#   * HammerDB's quackoltp.tcl: the same transactions with literals
#     interpolated, on HammerDB's schema.
#
# Each SCRIPT is (name, schema, steps).  A step is (sql, params, mode):
#   mode "o"  ordered compare (the statement has a total ORDER BY)
#   mode "u"  unordered compare (a multiset)
#   mode "e"  both engines must ERROR; the sqlite message must match
#             sqlxtc's verbatim (the UNIQUE constraint text)
# Every step runs on both engines in order, so DML results are checked
# through the reads that follow them.  Any mismatch -- including sqlxtc
# rejecting a statement sqlite runs -- is a FAIL.
#
# Seek-vs-scan evidence (rows visited per point statement) is asserted
# by the C test examples/06_sqlxtc/test_tpcc_seek.c; this file is the
# correctness gate.

import os
import sqlite3
import sys

sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
from compat_common import normalise_rows, Server  # noqa: E402

# ---- Turso schema (create_table_sqlite.sql + add_fkey_idx_sqlite.sql) ----
TURSO_SCHEMA = [
    "create table warehouse (w_id integer not null, w_name text, "
    "w_street_1 text, w_street_2 text, w_city text, w_state text, "
    "w_zip text, w_tax real, w_ytd real, primary key (w_id))",
    "create table district (d_id integer not null, d_w_id integer not null, "
    "d_name text, d_street_1 text, d_street_2 text, d_city text, "
    "d_state text, d_zip text, d_tax real, d_ytd real, d_next_o_id integer, "
    "primary key (d_w_id, d_id))",
    "create table customer (c_id integer not null, c_d_id integer not null, "
    "c_w_id integer not null, c_first text, c_middle text, c_last text, "
    "c_street_1 text, c_street_2 text, c_city text, c_state text, "
    "c_zip text, c_phone text, c_since text, c_credit text, "
    "c_credit_lim integer, c_discount real, c_balance real, "
    "c_ytd_payment real, c_payment_cnt integer, c_delivery_cnt integer, "
    "c_data text, primary key(c_w_id, c_d_id, c_id))",
    "create table history (h_c_id integer, h_c_d_id integer, "
    "h_c_w_id integer, h_d_id integer, h_w_id integer, h_date text, "
    "h_amount real, h_data text)",
    "create table new_orders (no_o_id integer not null, "
    "no_d_id integer not null, no_w_id integer not null, "
    "primary key(no_w_id, no_d_id, no_o_id))",
    "create table orders (o_id integer not null, o_d_id integer not null, "
    "o_w_id integer not null, o_c_id integer, o_entry_d text, "
    "o_carrier_id integer, o_ol_cnt integer, o_all_local integer, "
    "primary key(o_w_id, o_d_id, o_id))",
    "create table order_line (ol_o_id integer not null, "
    "ol_d_id integer not null, ol_w_id integer not null, "
    "ol_number integer not null, ol_i_id integer, ol_supply_w_id integer, "
    "ol_delivery_d text, ol_quantity integer, ol_amount real, "
    "ol_dist_info text, primary key(ol_w_id, ol_d_id, ol_o_id, ol_number))",
    "create table item (i_id integer not null, i_im_id integer, "
    "i_name text, i_price real, i_data text, primary key(i_id))",
    "create table stock (s_i_id integer not null, s_w_id integer not null, "
    "s_quantity integer, s_dist_01 text, s_dist_02 text, s_dist_03 text, "
    "s_dist_04 text, s_dist_05 text, s_dist_06 text, s_dist_07 text, "
    "s_dist_08 text, s_dist_09 text, s_dist_10 text, s_ytd real, "
    "s_order_cnt integer, s_remote_cnt integer, s_data text, "
    "primary key(s_w_id, s_i_id))",
    "CREATE INDEX IF NOT EXISTS idx_customer ON customer "
    "(c_w_id,c_d_id,c_last,c_first)",
    "CREATE INDEX IF NOT EXISTS idx_orders ON orders "
    "(o_w_id,o_d_id,o_c_id,o_id)",
    "CREATE INDEX IF NOT EXISTS fkey_stock_2 ON stock (s_i_id)",
    "CREATE INDEX IF NOT EXISTS fkey_order_line_2 ON order_line "
    "(ol_supply_w_id,ol_i_id)",
]

# ---- the 35 Turso statements (turso main.c:662-886), verbatim ----
T = [
    "SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse "
    "WHERE w_id = ? AND c_w_id = w_id AND c_d_id = ? AND c_id = ?",
    "SELECT d_next_o_id, d_tax FROM district WHERE d_id = ? AND d_w_id = ?",
    "UPDATE district SET d_next_o_id = ? + 1 WHERE d_id = ? AND d_w_id = ?",
    "INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, "
    "o_ol_cnt, o_all_local) VALUES(?, ?, ?, ?, ?, ?, ?)",
    "INSERT INTO new_orders (no_o_id, no_d_id, no_w_id) VALUES (?,?,?)",
    "SELECT i_price, i_name, i_data FROM item WHERE i_id = ?",
    "SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, "
    "s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, "
    "s_dist_10 FROM stock WHERE s_i_id = ? AND s_w_id = ?",
    "UPDATE stock SET s_quantity = ? WHERE s_i_id = ? AND s_w_id = ?",
    "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, "
    "ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
    "UPDATE warehouse SET w_ytd = w_ytd + ? WHERE w_id = ?",
    "SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name "
    "FROM warehouse WHERE w_id = ?",
    "UPDATE district SET d_ytd = d_ytd + ? WHERE d_w_id = ? AND d_id = ?",
    "SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM "
    "district WHERE d_w_id = ? AND d_id = ?",
    "SELECT count(c_id) FROM customer WHERE c_w_id = ? AND c_d_id = ? "
    "AND c_last = ?",
    "SELECT c_id FROM customer WHERE c_w_id = ? AND c_d_id = ? AND "
    "c_last = ? ORDER BY c_first",
    "SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, "
    "c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, "
    "c_balance, c_since FROM customer WHERE c_w_id = ? AND c_d_id = ? "
    "AND c_id = ?",
    "SELECT c_data FROM customer WHERE c_w_id = ? AND c_d_id = ? AND "
    "c_id = ?",
    "UPDATE customer SET c_balance = ?, c_data = ? WHERE c_w_id = ? AND "
    "c_d_id = ? AND c_id = ?",
    "UPDATE customer SET c_balance = ? WHERE c_w_id = ? AND c_d_id = ? "
    "AND c_id = ?",
    "INSERT INTO history(h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, "
    "h_date, h_amount, h_data) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
    "SELECT count(c_id) FROM customer WHERE c_w_id = ? AND c_d_id = ? "
    "AND c_last = ?",
    "SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE "
    "c_w_id = ? AND c_d_id = ? AND c_last = ? ORDER BY c_first",
    "SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE "
    "c_w_id = ? AND c_d_id = ? AND c_id = ?",
    "SELECT o_id, o_entry_d, COALESCE(o_carrier_id,0) FROM orders WHERE "
    "o_w_id = ? AND o_d_id = ? AND o_c_id = ? AND o_id = (SELECT "
    "MAX(o_id) FROM orders WHERE o_w_id = ? AND o_d_id = ? AND o_c_id = ?)",
    "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, "
    "ol_delivery_d FROM order_line WHERE ol_w_id = ? AND ol_d_id = ? AND "
    "ol_o_id = ?",
    "SELECT COALESCE(MIN(no_o_id),0) FROM new_orders WHERE no_d_id = ? "
    "AND no_w_id = ?",
    "DELETE FROM new_orders WHERE no_o_id = ? AND no_d_id = ? AND "
    "no_w_id = ?",
    "SELECT o_c_id FROM orders WHERE o_id = ? AND o_d_id = ? AND o_w_id = ?",
    "UPDATE orders SET o_carrier_id = ? WHERE o_id = ? AND o_d_id = ? "
    "AND o_w_id = ?",
    "UPDATE order_line SET ol_delivery_d = ? WHERE ol_o_id = ? AND "
    "ol_d_id = ? AND ol_w_id = ?",
    "SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = ? AND "
    "ol_d_id = ? AND ol_w_id = ?",
    "UPDATE customer SET c_balance = c_balance + ? , c_delivery_cnt = "
    "c_delivery_cnt + 1 WHERE c_id = ? AND c_d_id = ? AND c_w_id = ?",
    "SELECT d_next_o_id FROM district WHERE d_id = ? AND d_w_id = ?",
    "SELECT DISTINCT ol_i_id FROM order_line WHERE ol_w_id = ? AND "
    "ol_d_id = ? AND ol_o_id < ? AND ol_o_id >= (? - 20)",
    "SELECT count(*) FROM stock WHERE s_w_id = ? AND s_i_id = ? AND "
    "s_quantity < ?",
]
assert len(T) == 35

# Unordered for multiset results; "o" where ORDER BY is total in the
# data below (c_first values are distinct within a name group).
T_MODE = ["u"] * 35
T_MODE[14] = "o"
T_MODE[21] = "o"

LASTS = ["BARBARBAR", "BAROUGHTABLE", "ABLEPRIPRES", "OUGHTESEANTI"]


def turso_load():
    """A small but TPC-C-shaped dataset, with bound params (exercises the
    load path's INSERT ... VALUES(?,..) natively)."""
    st = []
    st.append(("INSERT INTO warehouse VALUES(?,?,?,?,?,?,?,?,?)",
               [1, "W1", "s1", "s2", "city", "ST", "12345", 0.1, 300000.0]))
    st.append(("INSERT INTO warehouse VALUES(?,?,?,?,?,?,?,?,?)",
               [2, "W2", "s1", "s2", "town", "XY", "54321", 0.2, 300000.0]))
    for w in (1, 2):
        for d in range(1, 4):
            st.append(("INSERT INTO district VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                       [d, w, "D%d" % d, "a", "b", "c", "S", "z", 0.05 * d,
                        30000.0, 31]))
    for i in range(1, 41):
        st.append(("INSERT INTO item VALUES(?,?,?,?,?)",
                   [i, i * 7, "item%d" % i, 1.5 * i, "data%d" % i]))
    for w in (1, 2):
        for i in range(1, 41):
            st.append(("INSERT INTO stock VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,"
                       "0,0,0,?)",
                       [i, w, (i * 13) % 90 + 10] +
                       ["dist%02d_%d" % (k, i) for k in range(1, 11)] +
                       ["sdata%d" % i]))
    for w in (1, 2):
        for d in range(1, 4):
            for c in range(1, 13):
                st.append(("INSERT INTO customer VALUES(?,?,?,?,?,?,?,?,?,?,"
                           "?,?,?,?,?,?,?, 10.0, 1, 0,?)",
                           [c, d, w, "F%02d" % ((c * 7) % 13), "OE",
                            LASTS[c % 4], "s1", "s2", "cty", "ST", "zip",
                            "555", "2026", "GC" if c % 3 else "BC", 50000,
                            0.01 * c, -10.0, "cdata%d" % c]))
    for w in (1, 2):
        for d in range(1, 4):
            for o in range(1, 31):
                cid = (o * 5) % 12 + 1
                if o > 20:
                    st.append(("INSERT INTO orders VALUES(?,?,?,?,?,NULL,?, 1)",
                               [o, d, w, cid, "2026-01-01", 5]))
                    st.append(("INSERT INTO new_orders VALUES(?,?,?)",
                               [o, d, w]))
                else:
                    st.append(("INSERT INTO orders VALUES(?,?,?,?,?,?,?, 1)",
                               [o, d, w, cid, "2026-01-01", o % 10 + 1, 5]))
                for ln in range(1, 6):
                    st.append(("INSERT INTO order_line VALUES(?,?,?,?,?,?,"
                               "?,?,?,?)",
                               [o, d, w, ln, (o * ln) % 40 + 1, w,
                                None if o > 20 else "2026-01-02", 5,
                                0.0 if o <= 20 else 1.25 * ln,
                                "info%d" % ln]))
    return st


def tq(i, params):
    return (T[i], params, T_MODE[i])


def turso_steps():
    s = []
    # ---- every statement individually (read before/after writes) ----
    s += [tq(0, [1, 2, 3]), tq(0, [2, 3, 12]), tq(0, [1, 2, 99])]
    s += [tq(1, [2, 1]), tq(1, [3, 2])]
    s += [tq(2, [31, 2, 1]), tq(1, [2, 1]), tq(32, [2, 1])]
    s += [tq(3, [31, 2, 1, 4, "2026-02-02", 7, 1]),
          tq(4, [31, 2, 1]),
          ("SELECT * FROM orders WHERE o_w_id=1 AND o_d_id=2 AND o_id=31",
           None, "u"),
          ("SELECT count(*) FROM new_orders", None, "u")]
    s += [tq(5, [7]), tq(5, [40]), tq(5, [41])]
    s += [tq(6, [7, 1]), tq(6, [40, 2]), tq(7, [33, 7, 1]), tq(6, [7, 1])]
    s += [tq(8, [31, 2, 1, 1, 7, 1, 5, 12.5, "dd"]), tq(24, [1, 2, 31])]
    s += [tq(9, [12.5, 1]), tq(10, [1]),
          ("SELECT w_ytd FROM warehouse", None, "u")]
    s += [tq(11, [7.25, 1, 2]), tq(12, [1, 2]),
          ("SELECT d_id, d_w_id, d_ytd FROM district", None, "u")]
    for lst in LASTS:
        s += [tq(13, [1, 2, lst]), tq(14, [1, 2, lst]), tq(20, [2, 3, lst]),
              tq(21, [2, 3, lst])]
    s += [tq(13, [1, 2, "NOSUCH"]), tq(14, [1, 2, "NOSUCH"])]
    s += [tq(15, [1, 2, 3]), tq(16, [2, 3, 4]), tq(22, [1, 1, 5])]
    # ordstat.c binds c_last (TEXT) into stmt 22's "c_id = ?": SQLite
    # applies the column's numeric affinity to the bind, a non-numeric
    # string stays TEXT and compares unequal (no row), "5" becomes 5.
    s += [tq(22, [1, 1, "BARBARBAR"]), tq(22, [1, 1, "5"]),
          ("SELECT c_id FROM customer WHERE c_w_id = 1 AND c_d_id = 1 AND "
           "c_id < 'x' AND c_id > '3'", None, "u"),
          ("SELECT count(*) FROM customer WHERE c_last = ?", [5], "u"),
          ("SELECT count(*) FROM customer WHERE c_last > ?", [5], "u")]
    s += [tq(17, [123.5, "newdata", 1, 2, 3]), tq(15, [1, 2, 3]),
          tq(16, [1, 2, 3])]
    s += [tq(18, [-5.5, 1, 2, 4]), tq(22, [1, 2, 4])]
    s += [tq(19, [2, 1, 3, 2, 1, "2026-03-03", 10.0, "hd"]),
          ("SELECT count(*), sum(h_amount) FROM history", None, "u")]
    for c in (1, 2, 5, 11):
        s += [tq(23, [1, 2, c, 1, 2, c])]
    s += [tq(23, [1, 2, 99, 1, 2, 99])]
    s += [tq(24, [2, 3, 7]), tq(24, [1, 1, 25])]
    s += [tq(25, [1, 1]), tq(25, [3, 2]), tq(26, [21, 1, 1]), tq(25, [1, 1])]
    s += [tq(27, [21, 1, 1]), tq(28, [4, 21, 1, 1]), tq(27, [21, 1, 1]),
          ("SELECT o_carrier_id FROM orders WHERE o_id=21 AND o_d_id=1 "
           "AND o_w_id=1", None, "u")]
    s += [tq(29, ["2026-04-04", 21, 1, 1]), tq(24, [1, 1, 21]),
          tq(30, [21, 1, 1]), tq(30, [25, 1, 1]), tq(30, [99, 1, 1])]
    s += [tq(31, [6.25, 3, 1, 1]),
          ("SELECT c_balance, c_delivery_cnt FROM customer WHERE c_w_id=1 "
           "AND c_d_id=1 AND c_id=3", None, "u")]
    s += [tq(33, [1, 2, 31, 31]), tq(33, [2, 3, 25, 25])]
    for i in (1, 5, 17, 40):
        s += [tq(34, [1, i, 50]), tq(34, [2, i, 20])]
    # ---- one New-Order + Payment + Delivery inside BEGIN..COMMIT ----
    s += [("BEGIN TRANSACTION", None, "u"),
          tq(0, [1, 3, 4]), tq(1, [3, 1]), tq(2, [31, 3, 1]),
          tq(3, [31, 3, 1, 4, "2026-05-05", 2, 1]), tq(4, [31, 3, 1]),
          tq(5, [9]), tq(6, [9, 1]), tq(7, [55, 9, 1]),
          tq(8, [31, 3, 1, 1, 9, 1, 3, 4.5, "x"]),
          tq(5, [10]), tq(6, [10, 1]), tq(7, [56, 10, 1]),
          tq(8, [31, 3, 1, 2, 10, 1, 3, 4.5, "y"]),
          # reads inside the txn see the txn's own writes
          tq(1, [3, 1]), tq(24, [1, 3, 31]), tq(25, [3, 1]),
          tq(23, [1, 3, 4, 1, 3, 4]),
          ("COMMIT", None, "u")]
    s += [("BEGIN TRANSACTION", None, "u"),
          tq(9, [100.0, 2]), tq(10, [2]), tq(11, [100.0, 2, 1]), tq(12, [2, 1]),
          tq(13, [2, 1, "BARBARBAR"]), tq(14, [2, 1, "BARBARBAR"]),
          tq(15, [2, 1, 4]), tq(18, [99.0, 2, 1, 4]),
          tq(19, [1, 2, 4, 1, 2, "2026-06-06", 100.0, "h"]),
          ("COMMIT", None, "u"),
          tq(22, [2, 1, 4])]
    s += [("BEGIN TRANSACTION", None, "u"),
          tq(25, [2, 2]), tq(26, [21, 2, 2]), tq(27, [21, 2, 2]),
          tq(28, [7, 21, 2, 2]), tq(29, ["2026-07-07", 21, 2, 2]),
          tq(30, [21, 2, 2]), tq(31, [6.25, 4, 2, 2]),
          tq(25, [2, 2]),
          ("COMMIT", None, "u"),
          ("SELECT * FROM new_orders WHERE no_w_id=2 AND no_d_id=2",
           None, "u")]
    # ---- composite PRIMARY KEY uniqueness (exact sqlite text) ----
    s += [tq(4, [22, 1, 1])]          # dup new_orders key (already exists)
    s[-1] = (T[4], [22, 1, 1], "e")
    s += [(T[3], [1, 1, 1, 1, "x", 1, 1], "e")]   # dup orders key
    s += [("BEGIN TRANSACTION", None, "u"),
          (T[4], [77, 1, 1], "u"),
          (T[4], [77, 1, 1], "e"),           # dup of own buffered write
          ("ROLLBACK", None, "u"),
          ("SELECT count(*) FROM new_orders WHERE no_o_id=77", None, "u")]
    # final state of every table
    for t in ("warehouse", "district", "customer", "history", "new_orders",
              "orders", "order_line", "item", "stock"):
        s += [("SELECT * FROM %s" % t, None, "u")]
    return s


# ---- HammerDB quackoltp.tcl (literals interpolated) ----
HDB_SCHEMA = [
    "CREATE TABLE customer (c_id INTEGER, c_d_id INTEGER, c_w_id INTEGER, "
    "c_first TEXT, c_middle TEXT, c_last TEXT, c_street_1 TEXT, "
    "c_street_2 TEXT, c_city TEXT, c_state TEXT, c_zip TEXT, c_phone TEXT, "
    "c_since INTEGER, c_credit TEXT, c_credit_lim REAL, c_discount REAL, "
    "c_balance REAL, c_data TEXT, c_ytd_payment REAL, c_payment_cnt INTEGER, "
    "c_delivery_cnt INTEGER, PRIMARY KEY (c_w_id, c_d_id, c_id))",
    "CREATE TABLE district (d_id INTEGER, d_w_id INTEGER, d_ytd REAL, "
    "d_tax REAL, d_next_o_id INTEGER, d_name TEXT, d_street_1 TEXT, "
    "d_street_2 TEXT, d_city TEXT, d_state TEXT, d_zip TEXT, "
    "PRIMARY KEY (d_w_id, d_id))",
    "CREATE TABLE history (h_c_id INTEGER, h_c_d_id INTEGER, "
    "h_c_w_id INTEGER, h_d_id INTEGER, h_w_id INTEGER, h_date INTEGER, "
    "h_amount REAL, h_data TEXT)",
    "CREATE TABLE item (i_id INTEGER PRIMARY KEY, i_im_id INTEGER, "
    "i_name TEXT, i_price REAL, i_data TEXT)",
    "CREATE TABLE new_order (no_w_id INTEGER, no_d_id INTEGER, "
    "no_o_id INTEGER, PRIMARY KEY (no_w_id, no_d_id, no_o_id))",
    "CREATE TABLE orders (o_id INTEGER, o_w_id INTEGER, o_d_id INTEGER, "
    "o_c_id INTEGER, o_carrier_id INTEGER, o_ol_cnt INTEGER, "
    "o_all_local INTEGER, o_entry_d INTEGER, "
    "PRIMARY KEY (o_w_id, o_d_id, o_id))",
    "CREATE TABLE order_line (ol_w_id INTEGER, ol_d_id INTEGER, "
    "ol_o_id INTEGER, ol_number INTEGER, ol_i_id INTEGER, "
    "ol_delivery_d INTEGER, ol_amount REAL, ol_supply_w_id INTEGER, "
    "ol_quantity INTEGER, ol_dist_info TEXT, "
    "PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number))",
    "CREATE TABLE stock (s_i_id INTEGER, s_w_id INTEGER, s_quantity INTEGER, "
    "s_dist_01 TEXT, s_dist_02 TEXT, s_dist_03 TEXT, s_dist_04 TEXT, "
    "s_dist_05 TEXT, s_dist_06 TEXT, s_dist_07 TEXT, s_dist_08 TEXT, "
    "s_dist_09 TEXT, s_dist_10 TEXT, s_ytd INTEGER, s_order_cnt INTEGER, "
    "s_remote_cnt INTEGER, s_data TEXT, PRIMARY KEY (s_w_id, s_i_id))",
    "CREATE TABLE warehouse (w_id INTEGER PRIMARY KEY, w_ytd REAL, "
    "w_tax REAL, w_name TEXT, w_street_1 TEXT, w_street_2 TEXT, "
    "w_city TEXT, w_state TEXT, w_zip TEXT)",
]


def hdb_load():
    st = []
    st.append(("INSERT INTO warehouse (w_id, w_ytd, w_tax, w_name, "
               "w_street_1, w_street_2, w_city, w_state, w_zip) VALUES "
               "(1, 3000000.00, .1, 'wn', 'a', 'b', 'c', 'ST', '1111')",
               None))
    vals = ",".join("(%d, 1, 30000.0, .05, 31, 'dn', 'a', 'b', 'c', 'S', 'z')"
                    % d for d in range(1, 4))
    st.append(("INSERT INTO district (d_id, d_w_id, d_ytd, d_tax, "
               "d_next_o_id, d_name, d_street_1, d_street_2, d_city, "
               "d_state, d_zip) VALUES " + vals, None))
    vals = ",".join("(%d, %d, 'it%d', %.2f, 'idata')" % (i, i, i, 1.5 * i)
                    for i in range(1, 31))
    st.append(("INSERT INTO item (i_id, i_im_id, i_name, i_price, i_data) "
               "VALUES " + vals, None))
    vals = ",".join("(%d, 1, %d, %s, 0, 0, 0, 'sd')"
                    % (i, (i * 13) % 90 + 10,
                       ", ".join("'d%02d'" % k for k in range(1, 11)))
                    for i in range(1, 31))
    st.append(("INSERT INTO stock (s_i_id, s_w_id, s_quantity, s_dist_01, "
               "s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, "
               "s_dist_07, s_dist_08, s_dist_09, s_dist_10, s_ytd, "
               "s_order_cnt, s_remote_cnt, s_data) VALUES " + vals, None))
    vals = ",".join(
        "(%d, %d, 1, 'F%02d', 'OE', '%s', 'a', 'b', 'c', 'S', 'z', '5', "
        "20260101, '%s', 50000, %.2f, -10.0, 'cd', 10.0, 1, 0)"
        % (c, d, (c * 7) % 13, LASTS[c % 4], "GC" if c % 3 else "BC",
           0.01 * c)
        for d in range(1, 4) for c in range(1, 13))
    st.append(("INSERT INTO customer (c_id, c_d_id, c_w_id, c_first, "
               "c_middle, c_last, c_street_1, c_street_2, c_city, c_state, "
               "c_zip, c_phone, c_since, c_credit, c_credit_lim, c_discount, "
               "c_balance, c_data, c_ytd_payment, c_payment_cnt, "
               "c_delivery_cnt) VALUES " + vals, None))
    for d in range(1, 4):
        vals = ",".join("(%d, %d, 1, %d, %s, 5, 1, 20260101)"
                        % (o, d, (o * 5) % 12 + 1,
                           "NULL" if o > 20 else str(o % 10 + 1))
                        for o in range(1, 31))
        st.append(("INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, "
                   "o_carrier_id, o_ol_cnt, o_all_local, o_entry_d) VALUES "
                   + vals, None))
        vals = ",".join("(%d, %d, 1)" % (o, d) for o in range(21, 31))
        st.append(("INSERT INTO new_order (no_o_id, no_d_id, no_w_id) "
                   "VALUES " + vals, None))
        vals = ",".join("(%d, %d, 1, %d, %d, 1, 5, %s, %.2f, 'od')"
                        % (o, d, ln, (o * ln) % 30 + 1,
                           "NULL" if o > 20 else "20260102",
                           0.0 if o <= 20 else 1.25 * ln)
                        for o in range(1, 31) for ln in range(1, 6))
        st.append(("INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, "
                   "ol_number, ol_i_id, ol_supply_w_id, ol_quantity, "
                   "ol_delivery_d, ol_amount, ol_dist_info) VALUES " + vals,
                   None))
    return st


def hdb_steps():
    u = "u"
    s = []
    # neword (quackoltp.tcl:513-560)
    s += [("SELECT customer.c_discount, customer.c_last, customer.c_credit, "
           "warehouse.w_tax FROM customer JOIN warehouse ON warehouse.w_id = "
           "customer.c_w_id WHERE customer.c_w_id = 1 AND customer.c_d_id = 2"
           " AND customer.c_id = 3", None, u),
          ("BEGIN", None, u),
          ("SELECT d_next_o_id, d_tax FROM district WHERE d_id = 2 AND "
           "d_w_id = 1", None, u),
          ("UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = 2"
           " AND d_w_id = 1", None, u),
          ("SELECT i_price, i_name, i_data FROM item WHERE i_id = 7", None, u),
          ("SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, "
           "s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09,"
           " s_dist_10 FROM stock WHERE s_i_id = 7 AND s_w_id = 1", None, u),
          ("UPDATE stock SET s_quantity = 44 WHERE s_i_id = 7 AND s_w_id = 1",
           None, u),
          ("INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, "
           "ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) "
           "VALUES (31, 2, 1, 1, 7, 1, 3, 12.25, 'd02')", None, u),
          ("INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, "
           "o_ol_cnt, o_all_local) VALUES (31, 2, 1, 3, 20260202, 1, 1)",
           None, u),
          ("INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES "
           "(31, 2, 1)", None, u),
          ("COMMIT", None, u)]
    # payment (591-627)
    s += [("BEGIN", None, u),
          ("UPDATE warehouse SET w_ytd = w_ytd + 77 WHERE w_id = 1", None, u),
          ("SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name "
           "FROM warehouse WHERE w_id = 1", None, u),
          ("UPDATE district SET d_ytd = d_ytd + 77 WHERE d_w_id = 1 AND "
           "d_id = 2", None, u),
          ("SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name "
           "FROM district WHERE d_w_id = 1 AND d_id = 2", None, u)]
    for lst in LASTS:
        s += [("SELECT count(c_id) FROM customer WHERE c_last = '%s' AND "
               "c_d_id = 2 AND c_w_id = 1" % lst, None, u),
              ("SELECT c_first, c_middle, c_id, c_street_1, c_street_2, "
               "c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, "
               "c_discount, c_balance, c_since FROM customer WHERE c_w_id = 1"
               " AND c_d_id = 2 AND c_last = '%s' ORDER BY c_first" % lst,
               None, "o")]
    s += [("SELECT c_first, c_middle, c_last, c_street_1, c_street_2, "
           "c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, "
           "c_discount, c_balance, c_since FROM customer WHERE c_w_id = 1 "
           "AND c_d_id = 2 AND c_id = 5", None, u),
          ("SELECT c_data FROM customer WHERE c_w_id = 1 AND c_d_id = 2 AND "
           "c_id = 5", None, u),
          ("UPDATE customer SET c_balance = 67.0, c_data = 'nd' WHERE "
           "c_w_id = 1 AND c_d_id = 2 AND c_id = 5", None, u),
          ("UPDATE customer SET c_balance = 68.0 WHERE c_w_id = 1 AND "
           "c_d_id = 2 AND c_id = 6", None, u),
          ("INSERT INTO history (h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id,"
           " h_date, h_amount, h_data) VALUES (2, 1, 5, 2, 1, 20260303, 77, "
           "'wn dn')", None, u),
          ("COMMIT", None, u)]
    # ostat (642-675)
    s += [("BEGIN", None, u),
          ("SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE "
           "c_last = 'BARBARBAR' AND c_d_id = 2 AND c_w_id = 1 ORDER BY "
           "c_first", None, "o"),
          ("SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE "
           "c_id = 5 AND c_d_id = 2 AND c_w_id = 1", None, u),
          ("SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = 2"
           " AND o_w_id = 1 AND o_c_id = 6 ORDER BY o_id DESC LIMIT 1",
           None, "o"),
          ("SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, "
           "ol_delivery_d FROM order_line WHERE ol_o_id = 7 AND ol_d_id = 2 "
           "AND ol_w_id = 1", None, u),
          ("COMMIT", None, u)]
    # delivery (669-677), two districts
    s += [("BEGIN", None, u)]
    for d in (1, 2):
        s += [("SELECT no_o_id FROM new_order WHERE no_w_id = 1 AND "
               "no_d_id = %d ORDER BY no_o_id LIMIT 1" % d, None, "o"),
              ("DELETE FROM new_order WHERE no_w_id = 1 AND no_d_id = %d AND"
               " no_o_id = 21" % d, None, u),
              ("SELECT o_c_id FROM orders WHERE o_id = 21 AND o_d_id = %d AND"
               " o_w_id = 1" % d, None, u),
              ("UPDATE orders SET o_carrier_id = 3 WHERE o_id = 21 AND "
               "o_d_id = %d AND o_w_id = 1" % d, None, u),
              ("UPDATE order_line SET ol_delivery_d = 20260404 WHERE "
               "ol_o_id = 21 AND ol_d_id = %d AND ol_w_id = 1" % d, None, u),
              ("SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = 21 AND"
               " ol_d_id = %d AND ol_w_id = 1" % d, None, u),
              ("UPDATE customer SET c_balance = c_balance + 18.75 WHERE "
               "c_id = 10 AND c_d_id = %d AND c_w_id = 1" % d, None, u)]
    s += [("COMMIT", None, u)]
    # slev (688-695)
    s += [("BEGIN", None, u),
          ("SELECT d_next_o_id FROM district WHERE d_w_id = 1 AND d_id = 2",
           None, u),
          ("SELECT COUNT(DISTINCT s_i_id) FROM stock WHERE s_w_id = 1 AND "
           "s_quantity < 60 AND s_i_id IN (SELECT ol_i_id FROM order_line "
           "WHERE ol_w_id = 1 AND ol_d_id = 2 AND ol_o_id < 32 AND "
           "ol_o_id >= (32 - 20))", None, u),
          ("COMMIT", None, u)]
    # driver bookkeeping (718, 753-755)
    s += [("SELECT sum(d_next_o_id) FROM district", None, u),
          ("SELECT max(w_id) FROM warehouse", None, u),
          ("SELECT max(d_id) FROM district", None, u)]
    for t in ("warehouse", "district", "customer", "history", "new_order",
              "orders", "order_line", "item", "stock"):
        s += [("SELECT * FROM %s" % t, None, u)]
    return s


# ---- composite keys, UNIQUE, multi-column / DESC indexes, tuple order ----
KEYS_SCHEMA = [
    "CREATE TABLE k(a INTEGER, b TEXT, v REAL, PRIMARY KEY(a, b))",
    "CREATE TABLE u(id INTEGER PRIMARY KEY, e TEXT UNIQUE, x INT, y INT, "
    "UNIQUE(x, y))",
    "CREATE TABLE m(n BLOB, t TEXT, r REAL)",
    "CREATE INDEX m_n ON m(n)",
    "CREATE INDEX m_tn ON m(t DESC, n)",
    "CREATE TABLE ip(id INT PRIMARY KEY, v)",
]


def keys_load():
    st = []
    for a in range(1, 6):
        for b in ("n1", "n10", "n2", "", "a"):
            st.append(("INSERT INTO k VALUES(?,?,?)", [a, b, a * 1.5]))
    for i in range(1, 9):
        st.append(("INSERT INTO u VALUES(?,?,?,?)",
                   [i, "e%d" % i, i % 3, i // 3]))
    vals = [3, 3.0, -2, 2.5, -2.75, 9007199254740993, 9007199254740992,
            -9223372036854775807, 1e300, -1e300, 0, None, "s", "S", "", b"\x00\x01",
            b"", "~", 2, 1.5]
    for i, v in enumerate(vals):
        st.append(("INSERT INTO m VALUES(?,?,?)",
                   [v, ["x", "y", "z"][i % 3], i * 0.5]))
    st.append(("INSERT INTO ip VALUES(?,?)", [1, "one"]))
    return st


def keys_steps():
    u, o, e = "u", "o", "e"
    s = []
    s += [("SELECT a, b FROM k WHERE a = ? ORDER BY b", [2], o),
          ("SELECT a, b, v FROM k WHERE a = ? AND b = ?", [3, "n1"], u),
          ("SELECT a, b, v FROM k WHERE a = ? AND b = ?", [3, "n10"], u),
          ("SELECT count(*) FROM k WHERE a BETWEEN ? AND ?", [2, 4], u),
          ("INSERT INTO k VALUES(?,?,?)", [2, "n2", 0.0], e),
          ("INSERT INTO k VALUES(?,?,?)", [2, "n3", 0.0], u),
          ("UPDATE k SET b = ? WHERE a = ? AND b = ?", ["n1", 2, "n3"], e),
          ("UPDATE k SET b = ? WHERE a = ? AND b = ?", ["n4", 2, "n3"], u),
          ("SELECT a, b FROM k WHERE a = 2 ORDER BY b DESC", None, o),
          ("INSERT INTO u VALUES(?,?,?,?)", [20, "e3", 7, 7], e),
          ("INSERT INTO u VALUES(?,?,?,?)", [21, "e21", 1, 1], e),
          ("INSERT INTO u VALUES(?,?,?,?)", [22, None, None, 1], u),
          ("INSERT INTO u VALUES(?,?,?,?)", [23, None, None, 1], u),
          ("INSERT INTO u VALUES(?,?,?,?)", [3, "e99", 9, 9], e),
          ("UPDATE u SET e = 'e1' WHERE id = 2", None, e),
          ("UPDATE u SET e = 'e2', x = x WHERE id = 2", None, u),
          ("DELETE FROM u WHERE id = 1", None, u),
          ("INSERT INTO u VALUES(?,?,?,?)", [30, "e1", 1, 0], u),
          ("SELECT * FROM u ORDER BY id", None, o),
          ("BEGIN", None, u),
          ("INSERT INTO u VALUES(50,'e50',50,50)", None, u),
          ("INSERT INTO u VALUES(51,'e50',51,51)", None, e),
          ("SELECT id FROM u WHERE e = 'e50'", None, u),
          ("UPDATE u SET e = 'e51' WHERE id = 50", None, u),
          ("INSERT INTO u VALUES(51,'e50',51,51)", None, u),
          ("COMMIT", None, u),
          ("SELECT id, e FROM u WHERE id >= 50 ORDER BY id", None, o),
          # tuple order: mixed INT/REAL/TEXT/BLOB/NULL through the indexes
          ("SELECT t, r FROM m WHERE t = ? ORDER BY t DESC, n, r", ["y"], o),
          ("SELECT t, r FROM m ORDER BY t DESC, n, r", None, o),
          ("SELECT r FROM m WHERE typeof(n) <> 'blob' ORDER BY n, r", None, o),
          ("CREATE UNIQUE INDEX m_un ON m(n)", None, e),
          ("CREATE UNIQUE INDEX k_v ON k(v)", None, e),
          ("CREATE UNIQUE INDEX m_ur ON m(r)", None, u),
          ("INSERT INTO m VALUES(1, 'q', 0.5)", None, e),
          # INT PRIMARY KEY is not a rowid alias: still unique
          ("INSERT INTO ip VALUES(?,?)", [1, "dup"], e),
          ("INSERT INTO ip VALUES(?,?)", [2, "two"], u),
          ("SELECT * FROM ip ORDER BY id", None, o),
          ("DROP TABLE k", None, u),
          ("CREATE TABLE k(a, b, PRIMARY KEY(a, b))", None, u),
          ("INSERT INTO k VALUES(1, 1)", None, u),
          ("INSERT INTO k VALUES(1, 1)", None, e),
          ("SELECT * FROM k", None, u)]
    for t in ("u", "ip"):
        s += [("SELECT * FROM %s" % t, None, u)]
    s += [("SELECT t, r FROM m", None, u)]
    return s


# ---- access paths + read-your-writes (sqlite-audit gap #4, #8 items 5/6) ----
# Every read shape the access-path chooser serves (rowid range, index
# equality prefix + range, ORDER BY from the index, MIN/MAX from the
# first/last key, UPDATE/DELETE through an index) inside a transaction
# that has written the table -- including a buffered INSERT into a rowid
# HOLE below the highest committed rowid, which a scan used to append
# only past the committed range (and so missed).
RYW_SCHEMA = [
    "CREATE TABLE t(k INTEGER PRIMARY KEY, grp INTEGER, v TEXT)",
    "CREATE INDEX t_grp ON t(grp)",
    "CREATE TABLE c(a INTEGER, b INTEGER, n TEXT, x REAL, PRIMARY KEY(a, b))",
    "CREATE INDEX c_n ON c(a, n DESC)",
]


def ryw_load():
    st = []
    for k, g in ((1, 7), (2, 8), (4, 7), (6, 9), (8, None), (9, 7)):
        st.append(("INSERT INTO t VALUES(?,?,?)", [k, g, "v%d" % k]))
    for a in (1, 2, 3):
        for b in (1, 2, 4, 5):
            st.append(("INSERT INTO c VALUES(?,?,?,?)",
                       [a, b, None if b == 4 else "n%d" % (b * a % 5),
                        a * 1.5 + b]))
    return st


def ryw_steps():
    u, o = "u", "o"
    reads = [
        ("SELECT k FROM t", None, u),
        ("SELECT k, v FROM t WHERE k < ? ORDER BY k", [5], o),
        ("SELECT k FROM t WHERE k BETWEEN ? AND ? ORDER BY k DESC", [2, 8], o),
        ("SELECT k FROM t WHERE grp = ?", [7], u),
        ("SELECT k FROM t WHERE grp = ? ORDER BY k", [7], o),
        ("SELECT count(*), sum(k) FROM t WHERE grp = ?", [7], u),
        ("SELECT min(k), max(k) FROM t WHERE grp = ?", [7], u),
        ("SELECT min(grp), max(grp) FROM t", None, u),
        ("SELECT grp FROM t WHERE grp >= ? ORDER BY grp LIMIT 2", [7], o),
        ("SELECT k FROM t WHERE grp > 7 ORDER BY grp DESC, k DESC", None, o),
        ("SELECT a, b, n FROM c WHERE a = ? ORDER BY b", [2], o),
        ("SELECT b FROM c WHERE a = ? AND b > ? AND b <= ?", [1, 1, 4], u),
        ("SELECT n FROM c WHERE a = ? ORDER BY n DESC", [3], o),
        ("SELECT n FROM c WHERE a = ? ORDER BY n", [3], o),
        ("SELECT COALESCE(MIN(b),0), COALESCE(MAX(b),0) FROM c WHERE a = ?",
         [2], u),
        ("SELECT MAX(b) FROM c WHERE a = ? AND b < ?", [2, 5], u),
        ("SELECT MIN(n) FROM c WHERE a = ?", [1], u),
        ("SELECT MAX(n) FROM c WHERE a = ?", [1], u),
        ("SELECT count(*) FROM c WHERE a = ? AND b = ?", [9, 9], u),
        ("SELECT MIN(b) FROM c WHERE a = ?", [9], u),
    ]
    s = list(reads)
    s += [("BEGIN", None, u),
          ("INSERT INTO t VALUES(3, 7, 'hole')", None, u),
          ("INSERT INTO t VALUES(5, 8, 'hole2')", None, u),
          ("INSERT INTO t VALUES(20, 7, 'tail')", None, u),
          ("INSERT INTO c VALUES(2, 3, 'n0', 0.5)", None, u),
          ("INSERT INTO c VALUES(2, 0, NULL, 0.25)", None, u)]
    s += reads
    s += [("UPDATE t SET grp = 9 WHERE k = 1", None, u),
          ("UPDATE t SET grp = 7 WHERE grp = 8", None, u),
          ("DELETE FROM t WHERE k = 4", None, u),
          ("DELETE FROM c WHERE a = 2 AND b = 5", None, u),
          ("UPDATE c SET n = 'zz' WHERE a = 3 AND b = 2", None, u)]
    s += reads
    s += [("COMMIT", None, u)]
    s += reads
    s += [("BEGIN", None, u),
          ("DELETE FROM t WHERE grp = 7", None, u),
          ("INSERT INTO t VALUES(4, 7, 'back')", None, u)]
    s += reads
    s += [("ROLLBACK", None, u)]
    s += reads
    return s


# ---- index nested-loop joins (sqlite-audit gap #6, #8 item 6) ----
JOIN_SCHEMA = [
    "CREATE TABLE w(w_id INTEGER PRIMARY KEY, tax REAL)",
    "CREATE TABLE c(c_id INTEGER, c_w INTEGER, c_d INTEGER, name TEXT, "
    "PRIMARY KEY(c_w, c_d, c_id))",
    "CREATE TABLE o(o_id INTEGER PRIMARY KEY, o_c INTEGER, o_w INTEGER, "
    "amt REAL)",
    "CREATE INDEX o_cw ON o(o_w, o_c)",
    "CREATE TABLE z(k INTEGER, v TEXT)",
]


def join_load():
    st = []
    for w in (1, 2, 3):
        st.append(("INSERT INTO w VALUES(?,?)", [w, 0.1 * w]))
    for w in (1, 2, 4):
        for d in (1, 2):
            for cid in (1, 2, 3):
                st.append(("INSERT INTO c VALUES(?,?,?,?)",
                           [cid, w, d, "n%d%d%d" % (w, d, cid)]))
    for i in range(1, 25):
        st.append(("INSERT INTO o VALUES(?,?,?,?)",
                   [i, i % 4, i % 3 + 1, 1.5 * i]))
    st.append(("INSERT INTO o VALUES(?,?,?,?)", [99, None, 1, 0.0]))
    for k in (1, 2, 2, None):
        st.append(("INSERT INTO z VALUES(?,?)", [k, "z%r" % k]))
    return st


def join_steps():
    u, o = "u", "o"
    reads = [
        # Turso stmt 0 shape: unqualified, outer filtered to a point
        ("SELECT name, tax FROM c, w WHERE w_id = ? AND c_w = w_id AND "
         "c_d = ? AND c_id = ?", [1, 2, 3], u),
        ("SELECT name, tax FROM c, w WHERE w_id = ? AND c_w = w_id AND "
         "c_d = ? AND c_id = ?", [4, 1, 1], u),
        ("SELECT name, tax FROM w, c WHERE c_w = w_id AND c_d = ?", [2], u),
        ("SELECT w.w_id, c.name FROM w JOIN c ON c.c_w = w.w_id "
         "WHERE c.c_id = ?", [2], u),
        ("SELECT a.w_id, b.name FROM w a JOIN c b ON b.c_w = a.w_id "
         "WHERE a.w_id >= ? ORDER BY b.name DESC", [2], o),
        ("SELECT c.name, o.o_id FROM c JOIN o ON o.o_w = c.c_w "
         "WHERE o.o_c = c.c_id AND c.c_d = ?", [1], u),
        ("SELECT c.name, o.o_id, w.tax FROM w JOIN c ON c.c_w = w.w_id "
         "JOIN o ON o.o_w = c.c_w WHERE c.c_d = ? AND c.c_id = ? AND "
         "o.amt > ?", [1, 1, 20.0], u),
        ("SELECT o.o_id, z.v FROM o JOIN z ON z.k = o.o_c", None, u),
        ("SELECT z.v, o.o_id FROM z JOIN o ON o.o_id = z.k", None, u),
        ("SELECT o.o_id, w.tax FROM o, w WHERE w.w_id = o.o_w AND "
         "o.o_id BETWEEN ? AND ?", [5, 9], u),
        ("SELECT count(*) FROM o", None, u),
    ]
    s = list(reads)
    s += [("INSERT INTO c VALUES(3, 1, 2, 'new')", None, u),
          ("INSERT INTO w VALUES(4, 0.4)", None, u),
          ("INSERT INTO o VALUES(50, 1, 1, 33.0)", None, u),
          ("INSERT INTO z VALUES(50, 'z50')", None, u),
          ("UPDATE c SET name = 'upd' WHERE c_w = 1 AND c_d = 1 AND c_id = 1",
           None, u),
          ("DELETE FROM o WHERE o_id = 4", None, u)]
    s += reads
    return s


SCRIPTS = [
    ("turso", TURSO_SCHEMA, turso_load, turso_steps),
    ("hammerdb", HDB_SCHEMA, hdb_load, hdb_steps),
    ("keys", KEYS_SCHEMA, keys_load, keys_steps),
    ("ryw", RYW_SCHEMA, ryw_load, ryw_steps),
    ("join", JOIN_SCHEMA, join_load, join_steps),
]


def ref_run(ref, sql, params):
    try:
        cur = ref.execute(sql, params or [])
        return cur.fetchall(), None
    except sqlite3.Error as e:
        return [], str(e)


def run_script(server, name, schema, load, steps, verbose):
    qk = server.fresh()
    ref = sqlite3.connect(":memory:")
    ref.isolation_level = None
    fails = 0
    nstep = 0
    for sql in schema:
        _, _, qe = qk.query(sql)
        _, re_ = ref_run(ref, sql, None)
        if qe is not None or re_ is not None:
            fails += 1
            print("  FAIL %s schema: %s\n       sqlxtc=%r sqlite=%r"
                  % (name, sql[:70], qe, re_))
    ref.execute("BEGIN")
    qk.query("BEGIN")
    for sql, params in load():
        _, _, qe = qk.query(sql, params)
        _, re_ = ref_run(ref, sql, params)
        if qe is not None or re_ is not None:
            fails += 1
            print("  FAIL %s load: %s %r\n       sqlxtc=%r sqlite=%r"
                  % (name, sql[:60], params, qe, re_))
            break
    qk.query("COMMIT")
    ref.execute("COMMIT")
    for sql, params, mode in steps():
        nstep += 1
        _, qrows, qe = qk.query(sql, params)
        rrows, re_ = ref_run(ref, sql, params)
        ok = True
        if mode == "e":
            ok = (qe is not None and re_ is not None and
                  re_ in qe)
        elif (qe is None) != (re_ is None):
            ok = False
        elif qe is None:
            ok = (normalise_rows(qrows, mode == "o") ==
                  normalise_rows(rrows, mode == "o"))
        if not ok:
            fails += 1
            print("  FAIL %s step %d: %s\n       params=%r\n"
                  "       sqlxtc: rows=%r err=%r\n"
                  "       sqlite: rows=%r err=%r"
                  % (name, nstep, sql, params, qrows[:4], qe, rrows[:4], re_))
            if qe is not None and re_ is None:
                # keep the two engines in step: the oracle applied it
                pass
        elif verbose:
            print("  ok   %s step %d" % (name, nstep))
    ref.close()
    return nstep, fails


def main():
    verbose = "-v" in sys.argv
    server = Server(port=16457)
    if not server.available():
        print("FAIL: sqlxtc-server not built at %s" % server.bin)
        return 1
    server.start()
    total = bad = 0
    try:
        for name, schema, load, steps in SCRIPTS:
            n, f = run_script(server, name, schema, load, steps, verbose)
            total += n
            bad += f
            print("tpcc %-9s: %d steps, %d failed" % (name, n, f))
    finally:
        server.stop()
    print("TPCC differential: %d/%d steps match sqlite" % (total - bad, total))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
