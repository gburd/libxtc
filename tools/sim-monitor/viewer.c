/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * tools/sim-monitor/viewer.c
 *	A VOPR-inspired (TigerBeetle) visualizer for libxtc's DST traces.
 *	Reads the xtc_tail v2 binary format (documented in
 *	src/inc/xtc_tail.h) and animates it: one lane per loop, procs as
 *	dots that move between run/parked, message sends as brief arrows,
 *	buggify activations as a flash on the affected lane.
 *
 *	This is a DEVELOPER TOOL, not part of the library: it parses the
 *	trace format directly (no libxtc dependency) and is not wired
 *	into make check, libxtc.a, or the default build/meson target --
 *	see tools/sim-monitor/README.md.  Playback is a fixed-rate replay
 *	of the recorded trace, not a live attach (see the README for why
 *	replay is the more useful mode for a deterministic simulator).
 *
 * Usage: viewer <trace-file>
 * Controls: SPACE pause/resume, LEFT/RIGHT step, UP/DOWN speed, ESC quit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "raylib.h"

#define XTC_TAIL_MAGIC   0x5854434Cu
#define XTC_TAIL_VERSION 2u

/* Mirrors src/inc/xtc_tail.h -- kept in sync by hand (this viewer has
 * no dependency on the library headers, deliberately: it is a
 * standalone consumer of the documented wire format). */
enum {
	SRC_SCHED = 1u << 0, SRC_MSG = 1u << 1, SRC_IO = 1u << 2,
	SRC_OS = 1u << 3, SRC_SIM = 1u << 4
};
enum {
	K_SPAWN = 0, K_EXIT = 1, K_WAKE = 2, K_RUN = 3, K_PARK = 4,
	K_SEND = 5, K_RECV = 6, K_MBOX_HWM = 7,
	K_BUGGIFY = 8, K_PARTITION = 9, K_MACHINE_DEATH = 10
};

typedef struct {
	uint64_t ts_ns;
	uint8_t  source, kind;
	uint16_t loop_id;
	uint16_t local_id;
	uint32_t gen;
	uint64_t detail;
} rec_t;

static uint64_t
read_le32(const uint8_t *p) { return (uint64_t)p[0] | (uint64_t)p[1] << 8 |
	(uint64_t)p[2] << 16 | (uint64_t)p[3] << 24; }
static uint64_t
read_le64(const uint8_t *p) {
	uint64_t lo = read_le32(p), hi = read_le32(p + 4);
	return lo | (hi << 32);
}
static uint64_t
read_leb128(const uint8_t *buf, size_t *off)
{
	uint64_t result = 0; int shift = 0; uint8_t b;
	do {
		b = buf[(*off)++];
		result |= (uint64_t)(b & 0x7f) << shift;
		shift += 7;
	} while (b & 0x80);
	return result;
}

static rec_t *
load_trace(const char *path, size_t *out_n)
{
	FILE *f = fopen(path, "rb");
	uint8_t hdr[24], *buf;
	long fsize;
	uint32_t magic, version, count;
	uint64_t base_ts;
	rec_t *recs;
	size_t off, i;
	uint64_t prev_ts;

	if (f == NULL) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
		fprintf(stderr, "trace too short\n"); exit(1);
	}
	magic = (uint32_t)read_le32(hdr + 0);
	version = (uint32_t)read_le32(hdr + 4);
	count = (uint32_t)read_le32(hdr + 12);
	base_ts = read_le64(hdr + 16);
	if (magic != XTC_TAIL_MAGIC || version != XTC_TAIL_VERSION) {
		fprintf(stderr, "bad trace header (magic=0x%x version=%u)\n",
		    magic, version);
		exit(1);
	}

	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	fseek(f, sizeof hdr, SEEK_SET);
	buf = malloc((size_t)fsize);
	if (fread(buf, 1, (size_t)fsize - sizeof hdr, f) !=
	    (size_t)fsize - sizeof hdr) {
		fprintf(stderr, "short read\n"); exit(1);
	}
	fclose(f);

	recs = calloc(count, sizeof *recs);
	off = 0;
	prev_ts = base_ts;
	for (i = 0; i < count; i++) {
		uint8_t kind = buf[off++];
		uint8_t source = buf[off++];
		uint64_t dts = read_leb128(buf, &off);
		uint64_t loop_id = read_leb128(buf, &off);
		uint64_t local_id = read_leb128(buf, &off);
		uint64_t gen = read_leb128(buf, &off);
		uint64_t detail = read_leb128(buf, &off);

		prev_ts += dts;
		recs[i].ts_ns = prev_ts;
		recs[i].source = source;
		recs[i].kind = kind;
		recs[i].loop_id = (uint16_t)loop_id;
		recs[i].local_id = (uint16_t)local_id;
		recs[i].gen = (uint32_t)gen;
		recs[i].detail = detail;
	}
	free(buf);
	*out_n = count;
	return recs;
}

#define MAX_LOOPS   16
#define MAX_FLASH   64
#define LANE_H      60
#define LEFT_MARGIN 90

typedef struct { int loop_id; float t; Color color; char label[32]; } flash_t;

int
main(int argc, char **argv)
{
	rec_t *recs; size_t n, cursor = 0;
	int n_loops = 1;
	int i;
	double speed = 1.0;    /* trace-seconds per real-second */
	int paused = 0;
	uint64_t t0, playhead_ns;
	double real_t0;
	flash_t flashes[MAX_FLASH]; int n_flash = 0;
	/* Per-loop dot: a little population indicator (spawns - exits),
	 * decayed toward 0 so a burst is visible then settles. */
	float lane_activity[MAX_LOOPS] = {0};

	if (argc != 2) {
		fprintf(stderr, "usage: %s <trace-file>\n", argv[0]);
		return 2;
	}
	recs = load_trace(argv[1], &n);
	if (n == 0) {
		fprintf(stderr, "empty trace\n");
		return 1;
	}
	for (i = 0; i < (int)n; i++)
		if (recs[i].loop_id + 1 > n_loops && recs[i].loop_id < MAX_LOOPS)
			n_loops = recs[i].loop_id + 1;
	t0 = recs[0].ts_ns;

	SetTraceLogLevel(LOG_WARNING);
	InitWindow(1000, LEFT_MARGIN / 2 + n_loops * LANE_H + 120,
	    "xtc sim-monitor -- DST trace replay");
	SetTargetFPS(60);

	real_t0 = GetTime();
	playhead_ns = 0;

	while (!WindowShouldClose()) {
		if (IsKeyPressed(KEY_SPACE)) paused = !paused;
		if (IsKeyPressed(KEY_UP)) speed *= 1.5;
		if (IsKeyPressed(KEY_DOWN)) speed /= 1.5;
		if (IsKeyPressed(KEY_RIGHT) && cursor < n) {
			playhead_ns = recs[cursor].ts_ns - t0;
			paused = 1;
		}
		if (IsKeyPressed(KEY_LEFT) && cursor > 0) {
			cursor--;
			if (cursor > 0) playhead_ns = recs[cursor - 1].ts_ns - t0;
		}

		if (!paused) {
			double now = GetTime();
			playhead_ns = (uint64_t)((now - real_t0) * speed * 1e9);
		} else {
			real_t0 = GetTime() - (double)playhead_ns / (speed * 1e9);
		}

		/* Advance the cursor, firing visuals for every record whose
		 * timestamp has now passed. */
		while (cursor < n && recs[cursor].ts_ns - t0 <= playhead_ns) {
			rec_t *r = &recs[cursor];
			if (r->loop_id < MAX_LOOPS) {
				if (r->kind == K_SPAWN) lane_activity[r->loop_id] += 1.0f;
				if (r->kind == K_EXIT)  lane_activity[r->loop_id] -= 1.0f;
				if (r->kind == K_RUN || r->kind == K_WAKE ||
				    r->source == SRC_MSG) {
					if (n_flash < MAX_FLASH) {
						flash_t *fl = &flashes[n_flash++];
						fl->loop_id = r->loop_id;
						fl->t = 0.25f;
						fl->color = (r->source == SRC_MSG) ?
						    (Color){80, 180, 255, 255} :
						    (Color){120, 255, 120, 180};
						snprintf(fl->label, sizeof fl->label, "%s",
						    r->kind == K_SEND ? "send" :
						    r->kind == K_RECV ? "recv" :
						    r->kind == K_RUN  ? "run"  : "wake");
					}
				}
				if (r->source == SRC_SIM && r->kind == K_BUGGIFY &&
				    n_flash < MAX_FLASH) {
					flash_t *fl = &flashes[n_flash++];
					fl->loop_id = r->loop_id;
					fl->t = 1.2f;
					fl->color = (Color){255, 60, 60, 220};
					snprintf(fl->label, sizeof fl->label, "BUGGIFY");
				}
			}
			cursor++;
		}
		if (cursor >= n) paused = 1;

		/* Decay flashes and lane activity for the frame. */
		{
			float dt = GetFrameTime();
			int w = 0;
			for (i = 0; i < n_flash; i++) {
				flashes[i].t -= dt;
				if (flashes[i].t > 0) flashes[w++] = flashes[i];
			}
			n_flash = w;
			for (i = 0; i < n_loops; i++)
				lane_activity[i] *= (1.0f - dt * 0.8f);
		}

		BeginDrawing();
		ClearBackground((Color){12, 12, 20, 255});

		DrawText("xtc sim-monitor", 12, 10, 20, RAYWHITE);
		DrawText(TextFormat("SPACE pause  <- -> step  UP/DOWN speed(%.2fx)  "
		    "%s  event %zu/%zu", speed, paused ? "PAUSED" : "PLAYING",
		    cursor, n), 12, 34, 14, GRAY);

		for (i = 0; i < n_loops; i++) {
			int y = 70 + i * LANE_H;
			DrawText(TextFormat("loop %d", i), 10, y + LANE_H / 2 - 8, 16,
			    LIGHTGRAY);
			DrawRectangle(LEFT_MARGIN, y, 1000 - LEFT_MARGIN - 20, LANE_H - 8,
			    (Color){24, 24, 36, 255});
			DrawRectangleLines(LEFT_MARGIN, y, 1000 - LEFT_MARGIN - 20,
			    LANE_H - 8, (Color){50, 50, 70, 255});

			/* Activity dot: brighter/bigger with more net-spawned procs
			 * on this lane right now (a cheap "how busy" indicator,
			 * not a literal per-proc position -- Phase 2 would track
			 * real per-proc dots via a proc-id -> slot map). */
			{
				float act = lane_activity[i];
				int radius = (int)(4 + (act > 0 ? act : 0) * 2);
				if (radius > 22) radius = 22;
				DrawCircle(LEFT_MARGIN + 40, y + LANE_H / 2 - 4, (float)radius,
				    (Color){90, 200, 255, 200});
			}
		}

		for (i = 0; i < n_flash; i++) {
			flash_t *fl = &flashes[i];
			if (fl->loop_id >= n_loops) continue;
			int y = 70 + fl->loop_id * LANE_H;
			float alpha = fl->t;
			Color c = fl->color;
			c.a = (unsigned char)(c.a * (alpha > 1 ? 1 : alpha));
			DrawRectangle(LEFT_MARGIN, y, 1000 - LEFT_MARGIN - 20, LANE_H - 8, c);
			DrawText(fl->label, LEFT_MARGIN + 60, y + LANE_H / 2 - 8, 16,
			    (Color){20, 10, 10, 255});
		}

		EndDrawing();
	}

	CloseWindow();
	free(recs);
	return 0;
}
