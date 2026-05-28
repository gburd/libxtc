/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_redis/cmd.c
 *	Command dispatch table and handlers.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "xtc_int.h"

/* Local helper: xtc __os_clock_mono uses out-param style. */
static inline int64_t xtc_now_ns(void) {
	int64_t t; (void)__os_clock_mono(&t); return t;
}

/* Case-insensitive compare */
static int
strcasecmp_n(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t i;
	if (alen != blen)
		return alen < blen ? -1 : 1;
	for (i = 0; i < alen; i++) {
		int ca = tolower((unsigned char)a[i]);
		int cb = tolower((unsigned char)b[i]);
		if (ca != cb)
			return ca - cb;
	}
	return 0;
}

/* ----- Command handlers ----- */

static int
cmd_ping(cmd_ctx_t *ctx)
{
	if (ctx->argc > 1) {
		resp_write_bulk(ctx->out, ctx->argv[1].u.str.data,
		                ctx->argv[1].u.str.len);
	} else {
		resp_write_pong(ctx->out);
	}
	return 0;
}

static int
cmd_echo(cmd_ctx_t *ctx)
{
	resp_write_bulk(ctx->out, ctx->argv[1].u.str.data,
	                ctx->argv[1].u.str.len);
	return 0;
}

static int
cmd_quit(cmd_ctx_t *ctx)
{
	resp_write_ok(ctx->out);
	*ctx->quit_flag = 1;
	return 0;
}

static int
cmd_auth(cmd_ctx_t *ctx)
{
	(void)ctx;
	/* No-op: always succeed */
	resp_write_ok(ctx->out);
	return 0;
}

static int
cmd_select(cmd_ctx_t *ctx)
{
	(void)ctx;
	/* No-op: single database */
	resp_write_ok(ctx->out);
	return 0;
}

static int
cmd_get(cmd_ctx_t *ctx)
{
	const char *data;
	size_t len;

	db_read_begin(ctx->db);
	if (db_get(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	           &data, &len) == 0) {
		resp_write_bulk(ctx->out, data, len);
	} else {
		resp_write_bulk_null(ctx->out);
	}
	db_read_end(ctx->db);
	return 0;
}

static int
cmd_set(cmd_ctx_t *ctx)
{
	int64_t expire_ns = 0;
	int i;

	/* Parse options: EX seconds, PX milliseconds, NX, XX */
	for (i = 3; i < ctx->argc; i++) {
		const char *opt = ctx->argv[i].u.str.data;
		size_t olen = ctx->argv[i].u.str.len;

		if (strcasecmp_n(opt, olen, "EX", 2) == 0 && i + 1 < ctx->argc) {
			i++;
			expire_ns = xtc_now_ns() +
			    atoll(ctx->argv[i].u.str.data) * 1000000000LL;
		} else if (strcasecmp_n(opt, olen, "PX", 2) == 0 && i + 1 < ctx->argc) {
			i++;
			expire_ns = xtc_now_ns() +
			    atoll(ctx->argv[i].u.str.data) * 1000000LL;
		}
		/* NX/XX ignored for simplicity */
	}

	db_write_begin(ctx->db);
	if (db_set_ex(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	              ctx->argv[2].u.str.data, ctx->argv[2].u.str.len,
	              expire_ns) < 0) {
		db_write_end(ctx->db);
		resp_write_error_n(ctx->out, "OOM", "out of memory or key limit reached");
		return 0;
	}
	db_write_end(ctx->db);
	resp_write_ok(ctx->out);
	return 0;
}

static int
cmd_del(cmd_ctx_t *ctx)
{
	int i, count = 0;

	db_write_begin(ctx->db);
	for (i = 1; i < ctx->argc; i++) {
		count += db_del(ctx->db, ctx->argv[i].u.str.data,
		                ctx->argv[i].u.str.len);
	}
	db_write_end(ctx->db);
	resp_write_int(ctx->out, count);
	return 0;
}

static int
cmd_exists(cmd_ctx_t *ctx)
{
	int i, count = 0;

	db_read_begin(ctx->db);
	for (i = 1; i < ctx->argc; i++) {
		count += db_exists(ctx->db, ctx->argv[i].u.str.data,
		                   ctx->argv[i].u.str.len);
	}
	db_read_end(ctx->db);
	resp_write_int(ctx->out, count);
	return 0;
}

static int
cmd_incr(cmd_ctx_t *ctx)
{
	int64_t val;

	db_write_begin(ctx->db);
	val = db_incr(ctx->db, ctx->argv[1].u.str.data,
	              ctx->argv[1].u.str.len, 1);
	db_write_end(ctx->db);

	if (val == -1) {
		resp_write_error(ctx->out, "value is not an integer");
		return 0;
	}
	resp_write_int(ctx->out, val);
	return 0;
}

static int
cmd_decr(cmd_ctx_t *ctx)
{
	int64_t val;

	db_write_begin(ctx->db);
	val = db_incr(ctx->db, ctx->argv[1].u.str.data,
	              ctx->argv[1].u.str.len, -1);
	db_write_end(ctx->db);

	if (val == -1) {
		resp_write_error(ctx->out, "value is not an integer");
		return 0;
	}
	resp_write_int(ctx->out, val);
	return 0;
}

static int
cmd_incrby(cmd_ctx_t *ctx)
{
	int64_t delta, val;

	delta = atoll(ctx->argv[2].u.str.data);

	db_write_begin(ctx->db);
	val = db_incr(ctx->db, ctx->argv[1].u.str.data,
	              ctx->argv[1].u.str.len, delta);
	db_write_end(ctx->db);

	if (val == -1) {
		resp_write_error(ctx->out, "value is not an integer");
		return 0;
	}
	resp_write_int(ctx->out, val);
	return 0;
}

static int
cmd_decrby(cmd_ctx_t *ctx)
{
	int64_t delta, val;

	delta = atoll(ctx->argv[2].u.str.data);

	db_write_begin(ctx->db);
	val = db_incr(ctx->db, ctx->argv[1].u.str.data,
	              ctx->argv[1].u.str.len, -delta);
	db_write_end(ctx->db);

	if (val == -1) {
		resp_write_error(ctx->out, "value is not an integer");
		return 0;
	}
	resp_write_int(ctx->out, val);
	return 0;
}

static int
cmd_expire(cmd_ctx_t *ctx)
{
	int64_t secs, expire_ns;

	secs = atoll(ctx->argv[2].u.str.data);
	expire_ns = xtc_now_ns() + secs * 1000000000LL;

	db_write_begin(ctx->db);
	if (db_expire(ctx->db, ctx->argv[1].u.str.data,
	              ctx->argv[1].u.str.len, expire_ns) < 0) {
		db_write_end(ctx->db);
		resp_write_int(ctx->out, 0);
		return 0;
	}
	db_write_end(ctx->db);
	resp_write_int(ctx->out, 1);
	return 0;
}

static int
cmd_ttl(cmd_ctx_t *ctx)
{
	int ttl;

	db_read_begin(ctx->db);
	ttl = db_ttl(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len);
	db_read_end(ctx->db);
	resp_write_int(ctx->out, ttl);
	return 0;
}

static int
cmd_keys(cmd_ctx_t *ctx)
{
	const char *keys[1024];
	size_t lens[1024];
	int i, n;

	db_read_begin(ctx->db);
	n = db_keys(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	            keys, lens, 1024);
	db_read_end(ctx->db);

	resp_write_array(ctx->out, n);
	for (i = 0; i < n; i++)
		resp_write_bulk(ctx->out, keys[i], lens[i]);
	return 0;
}

static int
cmd_flushdb(cmd_ctx_t *ctx)
{
	db_write_begin(ctx->db);
	db_flushdb(ctx->db);
	db_write_end(ctx->db);
	resp_write_ok(ctx->out);
	return 0;
}

static int
cmd_dbsize(cmd_ctx_t *ctx)
{
	size_t count = db_key_count(ctx->db);
	resp_write_int(ctx->out, (int64_t)count);
	return 0;
}

/* ----- List commands ----- */

static int
cmd_lpush(cmd_ctx_t *ctx)
{
	int64_t len = 0;
	int i;

	db_write_begin(ctx->db);
	for (i = 2; i < ctx->argc; i++) {
		len = db_lpush(ctx->db, ctx->argv[1].u.str.data,
		               ctx->argv[1].u.str.len,
		               ctx->argv[i].u.str.data,
		               ctx->argv[i].u.str.len);
		if (len < 0) {
			db_write_end(ctx->db);
			resp_write_error_n(ctx->out, "OOM", "out of memory");
			return 0;
		}
	}
	db_write_end(ctx->db);
	resp_write_int(ctx->out, len);
	return 0;
}

static int
cmd_rpush(cmd_ctx_t *ctx)
{
	int64_t len = 0;
	int i;

	db_write_begin(ctx->db);
	for (i = 2; i < ctx->argc; i++) {
		len = db_rpush(ctx->db, ctx->argv[1].u.str.data,
		               ctx->argv[1].u.str.len,
		               ctx->argv[i].u.str.data,
		               ctx->argv[i].u.str.len);
		if (len < 0) {
			db_write_end(ctx->db);
			resp_write_error_n(ctx->out, "OOM", "out of memory");
			return 0;
		}
	}
	db_write_end(ctx->db);
	resp_write_int(ctx->out, len);
	return 0;
}

/* Pop callback state */
struct pop_state {
	resp_buf_t *out;
	int         found;
};

static void
pop_cb(const char *data, size_t len, void *user)
{
	struct pop_state *s = user;
	resp_write_bulk(s->out, data, len);
	s->found = 1;
}

static int
cmd_lpop(cmd_ctx_t *ctx)
{
	struct pop_state st = { .out = ctx->out, .found = 0 };

	db_write_begin(ctx->db);
	db_lpop(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	        pop_cb, &st);
	db_write_end(ctx->db);

	if (!st.found)
		resp_write_bulk_null(ctx->out);
	return 0;
}

static int
cmd_rpop(cmd_ctx_t *ctx)
{
	struct pop_state st = { .out = ctx->out, .found = 0 };

	db_write_begin(ctx->db);
	db_rpop(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	        pop_cb, &st);
	db_write_end(ctx->db);

	if (!st.found)
		resp_write_bulk_null(ctx->out);
	return 0;
}

static int
cmd_llen(cmd_ctx_t *ctx)
{
	size_t len;

	db_read_begin(ctx->db);
	len = db_llen(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len);
	db_read_end(ctx->db);
	resp_write_int(ctx->out, (int64_t)len);
	return 0;
}

static int
cmd_lrange(cmd_ctx_t *ctx)
{
	const char *data[1024];
	size_t lens[1024];
	int start, stop, n, i;

	start = atoi(ctx->argv[2].u.str.data);
	stop = atoi(ctx->argv[3].u.str.data);

	db_read_begin(ctx->db);
	n = db_lrange(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	              start, stop, data, lens, 1024);
	db_read_end(ctx->db);

	resp_write_array(ctx->out, n);
	for (i = 0; i < n; i++)
		resp_write_bulk(ctx->out, data[i], lens[i]);
	return 0;
}

/* ----- Hash commands ----- */

static int
cmd_hset(cmd_ctx_t *ctx)
{
	int i, added = 0;

	db_write_begin(ctx->db);
	for (i = 2; i + 1 < ctx->argc; i += 2) {
		int r = db_hset(ctx->db,
		                ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
		                ctx->argv[i].u.str.data, ctx->argv[i].u.str.len,
		                ctx->argv[i+1].u.str.data, ctx->argv[i+1].u.str.len);
		if (r < 0) {
			db_write_end(ctx->db);
			resp_write_error_n(ctx->out, "OOM", "out of memory");
			return 0;
		}
		added += r;
	}
	db_write_end(ctx->db);
	resp_write_int(ctx->out, added);
	return 0;
}

static int
cmd_hget(cmd_ctx_t *ctx)
{
	const char *data;
	size_t len;

	db_read_begin(ctx->db);
	if (db_hget(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	            ctx->argv[2].u.str.data, ctx->argv[2].u.str.len,
	            &data, &len) == 0) {
		resp_write_bulk(ctx->out, data, len);
	} else {
		resp_write_bulk_null(ctx->out);
	}
	db_read_end(ctx->db);
	return 0;
}

static int
cmd_hdel(cmd_ctx_t *ctx)
{
	int i, count = 0;

	db_write_begin(ctx->db);
	for (i = 2; i < ctx->argc; i++) {
		count += db_hdel(ctx->db,
		                 ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
		                 ctx->argv[i].u.str.data, ctx->argv[i].u.str.len);
	}
	db_write_end(ctx->db);
	resp_write_int(ctx->out, count);
	return 0;
}

static int
cmd_hkeys(cmd_ctx_t *ctx)
{
	const char *keys[1024];
	size_t lens[1024];
	int i, n;

	db_read_begin(ctx->db);
	n = db_hkeys(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	             keys, lens, 1024);
	db_read_end(ctx->db);

	resp_write_array(ctx->out, n);
	for (i = 0; i < n; i++)
		resp_write_bulk(ctx->out, keys[i], lens[i]);
	return 0;
}

static int
cmd_hvals(cmd_ctx_t *ctx)
{
	const char *vals[1024];
	size_t lens[1024];
	int i, n;

	db_read_begin(ctx->db);
	n = db_hvals(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	             vals, lens, 1024);
	db_read_end(ctx->db);

	resp_write_array(ctx->out, n);
	for (i = 0; i < n; i++)
		resp_write_bulk(ctx->out, vals[i], lens[i]);
	return 0;
}

static int
cmd_hgetall(cmd_ctx_t *ctx)
{
	const char *keys[512], *vals[512];
	size_t key_lens[512], val_lens[512];
	int i, n;

	db_read_begin(ctx->db);
	n = db_hgetall(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	               keys, key_lens, vals, val_lens, 512);
	db_read_end(ctx->db);

	resp_write_array(ctx->out, n * 2);
	for (i = 0; i < n; i++) {
		resp_write_bulk(ctx->out, keys[i], key_lens[i]);
		resp_write_bulk(ctx->out, vals[i], val_lens[i]);
	}
	return 0;
}

static int
cmd_hlen(cmd_ctx_t *ctx)
{
	size_t len;

	db_read_begin(ctx->db);
	len = db_hlen(ctx->db, ctx->argv[1].u.str.data, ctx->argv[1].u.str.len);
	db_read_end(ctx->db);
	resp_write_int(ctx->out, (int64_t)len);
	return 0;
}

/* ----- Info & admin commands ----- */

static int
cmd_info(cmd_ctx_t *ctx)
{
	char buf[2048];
	int n;
	size_t keys, mem;

	keys = db_key_count(ctx->db);
	mem = db_mem_used(ctx->db);

	n = snprintf(buf, sizeof buf,
	    "# Server\r\n"
	    "redis_version:xtc-0.1\r\n"
	    "redis_mode:standalone\r\n"
	    "# Clients\r\n"
	    "# Memory\r\n"
	    "used_memory:%zu\r\n"
	    "# Keyspace\r\n"
	    "db0:keys=%zu,expires=0\r\n",
	    mem, keys);

	(void)ctx;
	resp_write_bulk(ctx->out, buf, (size_t)n);
	return 0;
}

static int
cmd_command(cmd_ctx_t *ctx)
{
	int count;
	const cmd_entry_t *tbl = cmd_table(&count);
	int i;

	(void)ctx;
	resp_write_array(ctx->out, count);
	for (i = 0; i < count; i++) {
		/* Simplified: just name as bulk string */
		resp_write_array(ctx->out, 2);
		resp_write_bulk(ctx->out, tbl[i].name, strlen(tbl[i].name));
		resp_write_int(ctx->out, tbl[i].min_args);
	}
	return 0;
}

static int
cmd_cluster(cmd_ctx_t *ctx)
{
	/* CLUSTER NODES for single-node */
	if (ctx->argc >= 2 &&
	    strcasecmp_n(ctx->argv[1].u.str.data, ctx->argv[1].u.str.len,
	                 "NODES", 5) == 0) {
		/* Return single node info */
		const char *info = "xtcredis0000000000000000000000000000000000 "
		                   "127.0.0.1:6379@16379 myself,master - 0 0 1 "
		                   "connected 0-16383\n";
		resp_write_bulk(ctx->out, info, strlen(info));
		return 0;
	}
	resp_write_error(ctx->out, "CLUSTER commands not supported");
	return 0;
}

/* ----- Command table ----- */

static cmd_entry_t g_commands[] = {
	/* Connection */
	{ "PING",      cmd_ping,      1, 2,  CMD_READONLY | CMD_FAST },
	{ "ECHO",      cmd_echo,      2, 2,  CMD_READONLY | CMD_FAST },
	{ "QUIT",      cmd_quit,      1, 1,  CMD_READONLY | CMD_FAST },
	{ "AUTH",      cmd_auth,      2, 3,  CMD_FAST },
	{ "SELECT",    cmd_select,    2, 2,  CMD_FAST },

	/* String */
	{ "GET",       cmd_get,       2, 2,  CMD_READONLY | CMD_FAST },
	{ "SET",       cmd_set,       3, -1, CMD_WRITE },
	{ "DEL",       cmd_del,       2, -1, CMD_WRITE },
	{ "EXISTS",    cmd_exists,    2, -1, CMD_READONLY | CMD_FAST },
	{ "INCR",      cmd_incr,      2, 2,  CMD_WRITE | CMD_FAST },
	{ "DECR",      cmd_decr,      2, 2,  CMD_WRITE | CMD_FAST },
	{ "INCRBY",    cmd_incrby,    3, 3,  CMD_WRITE | CMD_FAST },
	{ "DECRBY",    cmd_decrby,    3, 3,  CMD_WRITE | CMD_FAST },

	/* Key expiry */
	{ "EXPIRE",    cmd_expire,    3, 3,  CMD_WRITE | CMD_FAST },
	{ "TTL",       cmd_ttl,       2, 2,  CMD_READONLY | CMD_FAST },

	/* Keyspace */
	{ "KEYS",      cmd_keys,      2, 2,  CMD_READONLY },
	{ "FLUSHDB",   cmd_flushdb,   1, 2,  CMD_WRITE },
	{ "DBSIZE",    cmd_dbsize,    1, 1,  CMD_READONLY | CMD_FAST },

	/* List */
	{ "LPUSH",     cmd_lpush,     3, -1, CMD_WRITE },
	{ "RPUSH",     cmd_rpush,     3, -1, CMD_WRITE },
	{ "LPOP",      cmd_lpop,      2, 3,  CMD_WRITE | CMD_FAST },
	{ "RPOP",      cmd_rpop,      2, 3,  CMD_WRITE | CMD_FAST },
	{ "LLEN",      cmd_llen,      2, 2,  CMD_READONLY | CMD_FAST },
	{ "LRANGE",    cmd_lrange,    4, 4,  CMD_READONLY },

	/* Hash */
	{ "HSET",      cmd_hset,      4, -1, CMD_WRITE },
	{ "HGET",      cmd_hget,      3, 3,  CMD_READONLY | CMD_FAST },
	{ "HDEL",      cmd_hdel,      3, -1, CMD_WRITE },
	{ "HKEYS",     cmd_hkeys,     2, 2,  CMD_READONLY },
	{ "HVALS",     cmd_hvals,     2, 2,  CMD_READONLY },
	{ "HGETALL",   cmd_hgetall,   2, 2,  CMD_READONLY },
	{ "HLEN",      cmd_hlen,      2, 2,  CMD_READONLY | CMD_FAST },

	/* Info & admin */
	{ "INFO",      cmd_info,      1, 2,  CMD_READONLY | CMD_ADMIN },
	{ "COMMAND",   cmd_command,   1, -1, CMD_READONLY | CMD_ADMIN },
	{ "CLUSTER",   cmd_cluster,   2, -1, CMD_READONLY | CMD_ADMIN },

	{ NULL,        NULL,          0, 0,  0 }
};

void cmd_init(void) { /* nothing to do */ }

const cmd_entry_t *
cmd_lookup(const char *name, size_t len)
{
	int i;
	for (i = 0; g_commands[i].name; i++) {
		if (strcasecmp_n(name, len, g_commands[i].name,
		                 strlen(g_commands[i].name)) == 0)
			return &g_commands[i];
	}
	return NULL;
}

int
cmd_execute(cmd_ctx_t *ctx)
{
	const cmd_entry_t *e;
	const char *name;
	size_t len;

	if (ctx->argc < 1) {
		resp_write_error(ctx->out, "empty command");
		return -1;
	}

	name = ctx->argv[0].u.str.data;
	len = ctx->argv[0].u.str.len;
	e = cmd_lookup(name, len);

	if (!e) {
		char err[128];
		snprintf(err, sizeof err, "unknown command '%.64s'", name);
		resp_write_error(ctx->out, err);
		return -1;
	}

	if (ctx->argc < e->min_args) {
		resp_write_error(ctx->out, "wrong number of arguments");
		return -1;
	}
	if (e->max_args > 0 && ctx->argc > e->max_args) {
		resp_write_error(ctx->out, "wrong number of arguments");
		return -1;
	}

	return e->handler(ctx);
}

int
cmd_count(void)
{
	int i;
	for (i = 0; g_commands[i].name; i++)
		;
	return i;
}

const cmd_entry_t *
cmd_table(int *count)
{
	*count = cmd_count();
	return g_commands;
}
