/*-
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/sqlxtc/pgwire.h
 *      PostgreSQL wire protocol v3 (FE/BE) minimal implementation.
 *
 *      Implements just enough for psql SimpleQuery mode:
 *        - StartupMessage parsing
 *        - Query ('Q') message parsing
 *        - AuthenticationOk, ParameterStatus, BackendKeyData
 *        - ReadyForQuery, RowDescription, DataRow
 *        - CommandComplete, ErrorResponse
 */

#ifndef SQLXTC_PGWIRE_H
#define SQLXTC_PGWIRE_H

#include <stddef.h>
#include <stdint.h>

/* Protocol version: 3.0 = 0x00030000 = 196608 */
#define PG_PROTOCOL_MAJOR    3
#define PG_PROTOCOL_MINOR    0
#define PG_PROTOCOL_VERSION  ((PG_PROTOCOL_MAJOR << 16) | PG_PROTOCOL_MINOR)

/* Cancel request code: 80877102 = 1234 << 16 | 5678 */
#define PG_CANCEL_REQUEST_CODE  80877102

/* SSL request code: 80877103 */
#define PG_SSL_REQUEST_CODE     80877103

/* Frontend message types (client -> server) */
#define PG_MSG_QUERY         'Q'
#define PG_MSG_TERMINATE     'X'
#define PG_MSG_PASSWORD      'p'
#define PG_MSG_PARSE         'P'
#define PG_MSG_BIND          'B'
#define PG_MSG_DESCRIBE      'D'
#define PG_MSG_EXECUTE       'E'
#define PG_MSG_SYNC          'S'
#define PG_MSG_FLUSH         'H'
#define PG_MSG_CLOSE         'C'

/* Backend message types (server -> client) */
#define PG_MSG_AUTH              'R'
#define PG_MSG_PARAM_STATUS      'S'
#define PG_MSG_BACKEND_KEY_DATA  'K'
#define PG_MSG_READY_FOR_QUERY   'Z'
#define PG_MSG_ROW_DESCRIPTION   'T'
#define PG_MSG_DATA_ROW          'D'
#define PG_MSG_COMMAND_COMPLETE  'C'
#define PG_MSG_ERROR_RESPONSE    'E'
#define PG_MSG_NOTICE_RESPONSE   'N'
#define PG_MSG_EMPTY_QUERY       'I'
#define PG_MSG_PARSE_COMPLETE    '1'
#define PG_MSG_BIND_COMPLETE     '2'
#define PG_MSG_NO_DATA           'n'

/* Authentication types */
#define PG_AUTH_OK               0
#define PG_AUTH_CLEARTEXT        3
#define PG_AUTH_MD5              5
#define PG_AUTH_SASL             10

/* Transaction status indicators */
#define PG_TXN_IDLE              'I'
#define PG_TXN_IN_TRANSACTION    'T'
#define PG_TXN_ERROR             'E'

/* Error/Notice field identifiers */
#define PG_ERRFIELD_SEVERITY     'S'
#define PG_ERRFIELD_SQLSTATE     'C'
#define PG_ERRFIELD_MESSAGE      'M'
#define PG_ERRFIELD_DETAIL       'D'
#define PG_ERRFIELD_HINT         'H'
#define PG_ERRFIELD_POSITION     'P'

/* Parser state */
typedef struct pgwire_parser {
    const char *buf;
    size_t      len;
    size_t      pos;
} pgwire_parser_t;

/* Startup message parsed result */
typedef struct pgwire_startup {
    int32_t     version;
    const char *user;
    const char *database;
    /* Other parameters ignored for simplicity */
} pgwire_startup_t;

/* Parse result codes */
typedef enum pgwire_err {
    PGWIRE_OK        =  0,
    PGWIRE_NEED_MORE = -1,
    PGWIRE_ERR_PROTO = -2,
    PGWIRE_ERR_INVAL = -3,
    PGWIRE_ERR_TOOBIG = -4,
    PGWIRE_SSL_REQUEST = -5,
    PGWIRE_CANCEL_REQUEST = -6
} pgwire_err_t;

/* Response buffer */
typedef struct pgwire_buf {
    char   *data;
    size_t  cap;
    size_t  len;
} pgwire_buf_t;

/* Column info for RowDescription */
typedef struct pgwire_column {
    const char *name;
    int32_t     table_oid;
    int16_t     col_attr;
    int32_t     type_oid;
    int16_t     type_size;
    int32_t     type_mod;
    int16_t     format;
} pgwire_column_t;

/* Parser functions */
void pgwire_parser_init(pgwire_parser_t *p, const char *buf, size_t len);
pgwire_err_t pgwire_parse_startup(pgwire_parser_t *p, pgwire_startup_t *out,
                                  size_t *consumed);
pgwire_err_t pgwire_parse_message(pgwire_parser_t *p, char *type_out,
                                  const char **payload_out, size_t *payload_len,
                                  size_t *consumed);

/* Response buffer functions */
void pgwire_buf_init(pgwire_buf_t *b, char *data, size_t cap);
void pgwire_buf_reset(pgwire_buf_t *b);
size_t pgwire_buf_avail(const pgwire_buf_t *b);

/* Response writing functions */
int pgwire_write_auth_ok(pgwire_buf_t *b);
int pgwire_write_param_status(pgwire_buf_t *b, const char *name, const char *val);
int pgwire_write_backend_key_data(pgwire_buf_t *b, int32_t pid, int32_t key);
int pgwire_write_ready_for_query(pgwire_buf_t *b, char txn_status);
int pgwire_write_row_description(pgwire_buf_t *b, const pgwire_column_t *cols,
                                 int n_cols);
int pgwire_write_data_row_start(pgwire_buf_t *b, int n_cols);
int pgwire_write_data_row_col(pgwire_buf_t *b, const char *val, int len);
int pgwire_write_data_row_col_null(pgwire_buf_t *b);
int pgwire_write_data_row_end(pgwire_buf_t *b, size_t start_pos);
int pgwire_write_command_complete(pgwire_buf_t *b, const char *tag);
int pgwire_write_error(pgwire_buf_t *b, const char *severity,
                       const char *sqlstate, const char *message);
int pgwire_write_empty_query(pgwire_buf_t *b);
int pgwire_write_ssl_decline(pgwire_buf_t *b);

/* Utility: network byte order */
static inline uint32_t pgwire_get_u32(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
           ((uint32_t)u[2] << 8)  | (uint32_t)u[3];
}

static inline uint16_t pgwire_get_u16(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    return ((uint16_t)u[0] << 8) | (uint16_t)u[1];
}

static inline void pgwire_put_u32(char *p, uint32_t v) {
    unsigned char *u = (unsigned char *)p;
    u[0] = (v >> 24) & 0xFF;
    u[1] = (v >> 16) & 0xFF;
    u[2] = (v >> 8) & 0xFF;
    u[3] = v & 0xFF;
}

static inline void pgwire_put_u16(char *p, uint16_t v) {
    unsigned char *u = (unsigned char *)p;
    u[0] = (v >> 8) & 0xFF;
    u[1] = v & 0xFF;
}

#endif /* SQLXTC_PGWIRE_H */
