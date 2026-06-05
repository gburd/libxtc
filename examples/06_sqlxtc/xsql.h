/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xsql.h
 *	Vendored-SQL-engine symbol map: renames the public sqlite3_*
 *	API and type names this example uses to the project-native
 *	xsql_* names, so no sqlite3_ identifier appears in sqlxtc's own
 *	code.  Token-precise #defines (the C preprocessor matches whole
 *	tokens), so they rename only the standalone sqlite3 handle type
 *	and the sqlite3_<name> public identifiers -- NEVER the vendored
 *	internals (sqlite3VdbeExec, sqlite3BtreeOpen, ...), and never
 *	string literals, so the SQL-visible and file-format identifiers
 *	(sqlite_master, sqlite_stat*, "SQLite format 3") are untouched
 *	and SQL/on-disk compatibility is preserved.  See
 *	docs/M_SQLXTC_NAMING.md.
 *
 *	This header is FORCE-INCLUDED (-include) into both the vendored
 *	sqlite3.c compile and the sqlxtc seam files, so a renamed
 *	definition and every reference resolve to the same xsql_ symbol.
 *
 *	The naming convergence here is interim: it removes the SQLite
 *	branding from sqlxtc's surface while the engine is still
 *	vendored; the symbols leave the tree entirely once the from-
 *	scratch engine retires sqlite3.c.
 */

#ifndef SQLXTC_XSQL_H
#define SQLXTC_XSQL_H

/* The database-handle type. */
#define sqlite3 xsql

#define sqlite3_bind_blob xsql_bind_blob
#define sqlite3_bind_double xsql_bind_double
#define sqlite3_bind_int64 xsql_bind_int64
#define sqlite3_bind_null xsql_bind_null
#define sqlite3_bind_parameter_count xsql_bind_parameter_count
#define sqlite3_bind_text xsql_bind_text
#define sqlite3_busy_handler xsql_busy_handler
#define sqlite3_changes64 xsql_changes64
#define sqlite3_clear_bindings xsql_clear_bindings
#define sqlite3_close xsql_close
#define sqlite3_column_blob xsql_column_blob
#define sqlite3_column_bytes xsql_column_bytes
#define sqlite3_column_count xsql_column_count
#define sqlite3_column_double xsql_column_double
#define sqlite3_column_int xsql_column_int
#define sqlite3_column_int64 xsql_column_int64
#define sqlite3_column_name xsql_column_name
#define sqlite3_column_text xsql_column_text
#define sqlite3_column_type xsql_column_type
#define sqlite3_config xsql_config
#define sqlite3_context xsql_context
#define sqlite3_create_function xsql_create_function
#define sqlite3_create_module_v2 xsql_create_module_v2
#define sqlite3_declare_vtab xsql_declare_vtab
#define sqlite3_errmsg xsql_errmsg
#define sqlite3_exec xsql_exec
#define sqlite3_file xsql_file
#define sqlite3_finalize xsql_finalize
#define sqlite3_free xsql_free
#define sqlite3_get_autocommit xsql_get_autocommit
#define sqlite3_index_info xsql_index_info
#define sqlite3_initialize xsql_initialize
#define sqlite3_int64 xsql_int64
#define sqlite3_io_methods xsql_io_methods
#define sqlite3_malloc xsql_malloc
#define sqlite3_mem_methods xsql_mem_methods
#define sqlite3_module xsql_module
#define sqlite3_mutex xsql_mutex
#define sqlite3_mutex_alloc xsql_mutex_alloc
#define sqlite3_mutex_enter xsql_mutex_enter
#define sqlite3_mutex_free xsql_mutex_free
#define sqlite3_mutex_leave xsql_mutex_leave
#define sqlite3_mutex_methods xsql_mutex_methods
#define sqlite3_open xsql_open
#define sqlite3_open_v2 xsql_open_v2
#define sqlite3_pcache xsql_pcache
#define sqlite3_pcache_methods2 xsql_pcache_methods2
#define sqlite3_pcache_page xsql_pcache_page
#define sqlite3_prepare_v2 xsql_prepare_v2
#define sqlite3_reset xsql_reset
#define sqlite3_result_blob xsql_result_blob
#define sqlite3_result_double xsql_result_double
#define sqlite3_result_int xsql_result_int
#define sqlite3_result_int64 xsql_result_int64
#define sqlite3_result_null xsql_result_null
#define sqlite3_result_text xsql_result_text
#define sqlite3_shutdown xsql_shutdown
#define sqlite3_step xsql_step
#define sqlite3_stmt xsql_stmt
#define sqlite3_user_data xsql_user_data
#define sqlite3_value xsql_value
#define sqlite3_value_blob xsql_value_blob
#define sqlite3_value_bytes xsql_value_bytes
#define sqlite3_value_double xsql_value_double
#define sqlite3_value_int xsql_value_int
#define sqlite3_value_int64 xsql_value_int64
#define sqlite3_value_text xsql_value_text
#define sqlite3_value_type xsql_value_type
#define sqlite3_vfs xsql_vfs
#define sqlite3_vfs_find xsql_vfs_find
#define sqlite3_vfs_register xsql_vfs_register
#define sqlite3_vtab xsql_vtab
#define sqlite3_vtab_cursor xsql_vtab_cursor

#endif /* SQLXTC_XSQL_H */
