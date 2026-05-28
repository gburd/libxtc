/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/cmd.h
 *	Command dispatch table and handlers.
 */

#ifndef REXIS_CMD_H
#define REXIS_CMD_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"
#include "db.h"

/* Command context passed to handlers */
typedef struct cmd_ctx {
	db_t        *db;
	resp_buf_t  *out;
	int          argc;
	resp_value_t *argv;

	/* Rate limiting state (per-server) */
	int64_t     *iops_tokens;
	int64_t      iops_cap;

	/* Connection state */
	int         *quit_flag;
} cmd_ctx_t;

/* Command flags */
#define CMD_READONLY   (1 << 0)    /* does not modify data */
#define CMD_WRITE      (1 << 1)    /* modifies data */
#define CMD_ADMIN      (1 << 2)    /* administrative command */
#define CMD_FAST       (1 << 3)    /* O(1) complexity */

typedef int (*cmd_handler_fn)(cmd_ctx_t *ctx);

typedef struct cmd_entry {
	const char      *name;
	cmd_handler_fn   handler;
	int              min_args;   /* including command name */
	int              max_args;   /* -1 = unlimited */
	unsigned         flags;
} cmd_entry_t;

/* Initialize command table */
void cmd_init(void);

/* Find command by name (case-insensitive) */
const cmd_entry_t *cmd_lookup(const char *name, size_t len);

/* Execute a command.  Returns 0 on success, -1 on error. */
int cmd_execute(cmd_ctx_t *ctx);

/* Get command count for INFO */
int cmd_count(void);

/* Get command table for COMMAND */
const cmd_entry_t *cmd_table(int *count);

#endif /* REXIS_CMD_H */
