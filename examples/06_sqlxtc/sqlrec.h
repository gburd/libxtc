/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sqlrec.h
 *	The sqlxtc record format and value type system.
 *
 *	sqlxtc stores rows in the SQLite record format so the data file
 *	and the SQL layer interoperate with the reference engine
 *	byte-for-byte.  This module is the standalone, self-contained
 *	encoder/decoder for that format -- the piece the vectorized
 *	executor will use to read column values out of a stored row
 *	without going through the VDBE.
 *
 *	Storage classes (SQLite's five) and their serial types:
 *	    NULL                 serial 0
 *	    INTEGER              serials 1,2,3,4,5,6 (1,2,3,4,6,8 bytes,
 *	                         big-endian two's complement) and the
 *	                         constants 8 (=0) and 9 (=1)
 *	    REAL                 serial 7 (big-endian IEEE-754 binary64)
 *	    TEXT                 odd serial N>=13, (N-13)/2 bytes
 *	    BLOB                 even serial N>=12, (N-12)/2 bytes
 *
 *	A record is a header followed by a body.  The header is a varint
 *	holding the total header length (including its own bytes) then one
 *	varint serial-type per column.  The body is the column blobs in
 *	order.  This matches sqlite3VdbeSerialType / serialGet exactly.
 */

#ifndef SQLXTC_SQLREC_H
#define SQLXTC_SQLREC_H

#include <stddef.h>
#include <stdint.h>

/* ---- varint (SQLite's 1..9 byte big-endian-grouped form) ---------- */

/* Encode v into buf (which must hold up to 9 bytes).  Returns the byte
 * count written (1..9).  Matches sqlite3PutVarint. */
int sqlrec_put_varint(uint8_t *buf, uint64_t v);

/* Decode a varint at p (reading at most `avail` bytes).  Stores the
 * value in *v and returns the byte count consumed (1..9), or 0 if the
 * buffer is too short for the encoding.  Matches sqlite3GetVarint. */
int sqlrec_get_varint(const uint8_t *p, size_t avail, uint64_t *v);

/* Number of bytes sqlrec_put_varint would write for v (1..9). */
int sqlrec_varint_len(uint64_t v);

/* ---- value type system (mirrors SQLite storage classes) ----------- */

typedef enum sqlrec_type {
	SQLREC_NULL = 0,
	SQLREC_INT,
	SQLREC_REAL,
	SQLREC_TEXT,
	SQLREC_BLOB
} sqlrec_type_t;

/* A decoded column value.  TEXT/BLOB point into the source record
 * buffer (not owned) -- the caller keeps that buffer alive while it
 * reads the value.  TEXT is NOT NUL-terminated; use .n. */
typedef struct sqlrec_value {
	sqlrec_type_t type;
	union {
		int64_t     i;     /* INT */
		double      r;     /* REAL */
		struct {           /* TEXT / BLOB */
			const uint8_t *p;
			uint32_t       n;
		} bytes;
	} u;
} sqlrec_value_t;

/* ---- serial types ------------------------------------------------- */

/* The serial type that encodes value v (matches sqlite3VdbeSerialType
 * with file_format>=4, so 0 and 1 integers use the constant types 8/9).
 * *out_len receives the number of body bytes the value occupies. */
uint64_t sqlrec_serial_type(const sqlrec_value_t *v, uint32_t *out_len);

/* Body byte count for a serial type (matches sqlite3VdbeSerialTypeLen). */
uint32_t sqlrec_serial_len(uint64_t serial_type);

/* Write value v's body bytes into buf (which must hold
 * sqlrec_serial_len(serial_type) bytes) for the given serial type.
 * Returns the byte count written. */
uint32_t sqlrec_serial_put(uint8_t *buf, const sqlrec_value_t *v,
                           uint64_t serial_type);

/* Decode the body bytes at p (length sqlrec_serial_len(serial_type))
 * under serial_type into *out.  TEXT/BLOB values point into p. */
void sqlrec_serial_get(const uint8_t *p, uint64_t serial_type,
                       sqlrec_value_t *out);

/* ---- whole-record encode / decode --------------------------------- */

/* Encode `nvals` values into a SQLite-format record written to `buf`.
 * If buf is NULL the bytes are not written but the required length is
 * still returned (size probe).  `cap` bounds writing.  Returns the total
 * record length, or -1 on overflow of cap (when buf != NULL). */
int sqlrec_encode(uint8_t *buf, size_t cap,
                  const sqlrec_value_t *vals, int nvals);

/* A cursor over the columns of an encoded record.  Decodes lazily:
 * sqlrec_reader_open parses only the header; sqlrec_reader_col(i)
 * decodes column i on demand. */
typedef struct sqlrec_reader {
	const uint8_t *rec;      /* whole record (not owned) */
	size_t         rec_len;
	const uint8_t *body;     /* start of the body (after the header) */
	const uint8_t *hdr;      /* start of the serial-type list */
	size_t         hdr_len;  /* total header length (incl. its size varint) */
	int            ncol;     /* number of columns */
	int            ok;       /* 0 if the record is malformed */
} sqlrec_reader_t;

/* Parse the header of `rec` (length rec_len).  Returns 0 on success
 * (reader->ncol set) or -1 if the record is malformed. */
int sqlrec_reader_open(sqlrec_reader_t *rd, const uint8_t *rec, size_t rec_len);

/* Column count of an opened reader. */
int sqlrec_reader_ncol(const sqlrec_reader_t *rd);

/* Decode column i (0-based) into *out.  Returns 0 on success, -1 if i
 * is out of range or the record is malformed. */
int sqlrec_reader_col(const sqlrec_reader_t *rd, int i, sqlrec_value_t *out);

#endif /* SQLXTC_SQLREC_H */
