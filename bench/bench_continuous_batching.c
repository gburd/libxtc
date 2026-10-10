/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_async.h"
#include "xtc_batch.h"
#include "xtc_proc.h"
#include "xtc_sched.h"

struct summary {
	uint64_t work, checksum, completed, calls, rejections;
	int refilled;
};
struct config {
	unsigned requests, active, rounds, spacing;
	int continuous, policy, workload;
};
static struct summary run(const struct config *);

static void
check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		exit(1);
	}
}

static void
self_test(void)
{
	struct config c = { 64, 4, 8, 0, 0, 0, 0 };
	struct summary base, got;
	int w, mode, policy;

	for (w = 0; w < 4; w++) {
		c.workload = w;
		c.continuous = c.policy = 0;
		base = run(&c);
		check(base.completed == c.requests && base.work != 0,
		      "all fixed work completed");
		for (mode = 0; mode < 2; mode++) {
			for (policy = 0; policy < 2; policy++) {
				c.continuous = mode;
				c.policy = policy;
				got = run(&c);
				check(
				    got.completed == c.requests
				        && got.work == base.work
				        && got.checksum == base.checksum,
				    "equal work and scalar results across arms");
				if (w == 3)
					check(
					    got.refilled == mode,
					    "refill before initial long request completes");
				if (w == 2 && mode)
					check(
					    got.rejections > 0,
					    "overload rejects and retries without loss");
			}
		}
	}
	puts("batch benchmark self-test: PASS");
}

/* Fixed request arithmetic, not a sleep-based simulation of compute. Arrival
 * ticks are owner yields, NOT wall-time offered rates. Saturated uses a bounded
 * closed-loop feeder; overload offers the whole backlog and retries AGAIN.
 * Static admits a cohort only after the previous cohort has entirely drained.
 * Continuous fills holes at boundaries. Both have identical resource bounds.
 */
#define WIDTH 16
#define TABLE 256
#define BINS 65
static const char *workloads[] = { "lowload", "saturated", "overload", "skew" };
struct histogram {
	uint64_t bins[BINS], count, max;
};
struct request {
	uint64_t values[WIDTH], expected[WIDTH], handle;
	uint64_t arrival, admitted, first, finished;
	unsigned length, rounds, terminals;
};
struct bench {
	struct config c;
	struct request *requests;
	xtc_batch_t *batch;
	struct summary out;
	struct histogram heartbeat;
	uint64_t start, end, members, peak_resources;
	unsigned live, terminal;
};

static void
must(int rc)
{
	if (rc != XTC_OK) {
		fprintf(stderr, "libxtc: %s (requires --enable-sched-policy)\n",
		        xtc_strerror(rc));
		exit(1);
	}
}

static uint64_t
now(void)
{
	int64_t t = xtc_clock_mono();
	check(t > 0, "monotonic clock");
	return (uint64_t)t;
}

/* Log2 upper-bound buckets in ns; maxima remain exact. */
static void
record(struct histogram *h, uint64_t ns)
{
	uint64_t v = ns;
	unsigned i = 0;

	while (v != 0) {
		i++;
		v >>= 1;
	}
	h->bins[i]++;
	h->count++;
	if (ns > h->max)
		h->max = ns;
}

static uint64_t
percentile(const struct histogram *h, unsigned permille)
{
	uint64_t count = 0, target = (h->count * permille + 999) / 1000;
	unsigned i;

	for (i = 0; i < BINS; i++) {
		count += h->bins[i];
		if (count >= target)
			return i == 64 ? UINT64_MAX : (UINT64_C(1) << i) - 1;
	}
	return 0;
}

static void
print_hist(const char *name, const struct histogram *h)
{
	printf("%s_ns n=%" PRIu64 " p50<=%" PRIu64 " p99<=%" PRIu64
	       " p99.9<=%" PRIu64 " max=%" PRIu64 "\n",
	       name, h->count, percentile(h, 500), percentile(h, 990),
	       percentile(h, 999), h->max);
}

/* Table construction is real shared setup consumed by the numerical kernel.
 * Each entry depends on its predecessor. No volatile busy loop or fake delay.
 * Rebuilding the table once per vector intentionally models noncached setup;
 * applications able to cache it across vectors will see different tradeoffs.
 */
static int
step(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
     void *arg)
{
	struct bench *b = arg;
	struct request *r;
	xtc_batch_stats_t stats = { .size = sizeof(stats),
		                    .version = XTC_BATCH_VERSION };
	uint64_t table[TABLE], x = UINT64_C(0x9e3779b97f4a7c15), t = now();
	size_t i;
	unsigned j;

	for (j = 0; j < TABLE; j++) {
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		table[j] = x * UINT64_C(2685821657736338717);
	}
	b->out.calls++;
	b->members += n;
	must(xtc_batch_get_stats(b->batch, &stats));
	if (stats.reserved_units > b->peak_resources)
		b->peak_resources = stats.reserved_units;
	for (i = 0; i < n; i++) {
		r = items[i].request;
		check(r->terminals == 0 && r->rounds < r->length,
		      "live vector member");
		if (!r->first) {
			r->first = t;
			if ((size_t)(r - b->requests) >= b->c.active
			    && !b->requests[0].terminals)
				b->out.refilled = 1;
		}
		for (j = 0; j < WIDTH; j++)
			r->values[j] =
			    r->values[j]
			        * (table[(r->rounds * WIDTH + j) % TABLE] | 1)
			    + r->rounds + 1;
		r->rounds++;
		b->out.work += WIDTH;
		results[i].state = r->rounds == r->length ? XTC_BATCH_DONE :
		                                            XTC_BATCH_CONTINUE;
	}
	return XTC_OK;
}

static void
complete(uint64_t handle, void *request, int outcome, void *arg)
{
	struct bench *b = arg;
	struct request *r = request;

	check(outcome == XTC_OK && handle == r->handle && ++r->terminals == 1,
	      "exactly one successful completion");
	r->finished = now();
	check(b->live > 0, "live conservation");
	b->live--;
	if (++b->terminal == b->c.requests)
		b->end = r->finished;
}

static void
heartbeat(void *arg)
{
	struct bench *b = arg;
	uint64_t t;

	while (!b->end) {
		t = now();
		xtc_yield();
		record(&b->heartbeat, now() - t);
	}
}

static void
owner(void *arg)
{
	struct bench *b = arg;
	struct request *r;
	uint64_t tick = 0, due, t;
	unsigned arrived = 0, next = 0, limit;
	int rc;

	b->start = now();
	while (b->terminal < b->c.requests) {
		t = now();
		while (arrived < b->c.requests) {
			due = b->c.workload == 0 ?
			          (uint64_t)arrived * b->c.spacing :
			          0;
			if (due > tick)
				break;
			b->requests[arrived++].arrival = t;
		}
		/* Static membership is frozen until this cohort is fully
		 * drained. */
		if (b->c.continuous || b->live == 0) {
			limit = b->c.continuous ? arrived : next + b->c.active;
			if (limit > arrived)
				limit = arrived;
			while (next < limit) {
				if (b->c.workload != 2
				    && b->live == 2 * b->c.active)
					break;
				r = &b->requests[next];
				rc = xtc_batch_submit(b->batch, r, 1,
				                      &r->handle);
				if (rc == XTC_E_AGAIN) {
					b->out.rejections++;
					break;
				}
				must(rc);
				r->admitted = now();
				b->live++;
				next++;
			}
		}
		/* Safety watchdog, not a performance threshold. */
		check(++tick < (uint64_t)b->c.requests
		                   * (b->c.spacing + b->c.rounds * 16 + 1) * 64,
		      "bounded owner progress");
		xtc_yield();
	}
	must(xtc_batch_close(b->batch, XTC_BATCH_DRAIN));
}

/* Independent scalar oracle: each component derives its needed coefficients
 * directly from the recurrence, then evaluates all rounds before the next
 * component. Executed outside the measurement window. */
static void
prepare(struct bench *b)
{
	struct request *r;
	uint64_t coefficients[TABLE], x, value;
	unsigned i, j, k, q;

	for (j = 0; j < TABLE; j++) {
		x = UINT64_C(0x9e3779b97f4a7c15);
		for (q = 0; q <= j; q++) {
			x = x ^ (x >> 12);
			x = x ^ (x << 25);
			x = x ^ (x >> 27);
		}
		coefficients[j] = (x * UINT64_C(2685821657736338717)) | 1;
	}
	for (i = 0; i < b->c.requests; i++) {
		r = &b->requests[i];
		r->length = b->c.workload == 3 ?
		                (i % b->c.active == 0 ? b->c.rounds * 16 : 1) :
		                1 + i % b->c.rounds;
		for (j = 0; j < WIDTH; j++) {
			value = UINT64_MAX - (uint64_t)i * WIDTH + j;
			r->values[j] = value;
			for (k = 0; k < r->length; k++)
				value = coefficients[(k * WIDTH + j) % TABLE]
				            * value
				        + k + 1;
			r->expected[j] = value;
		}
	}
}

static struct summary
run(const struct config *config)
{
	struct bench b = { .c = *config };
	struct histogram admission = { 0 }, first = { 0 }, completion = { 0 };
	struct request *r;
	xtc_loop_t *loop;
	xtc_exec_class_t cls;
	xtc_batch_opts_t opts = { .size = sizeof(opts),
		                  .version = XTC_BATCH_VERSION,
		                  .step = step,
		                  .complete = complete,
		                  .user = &b };
	xtc_batch_stats_t stats = { .size = sizeof(stats),
		                    .version = XTC_BATCH_VERSION };
	uint64_t expected_work = 0;
	unsigned i, j;
	double seconds;

	if (!b.c.spacing)
		b.c.spacing = b.c.rounds * 4;
	b.requests = xtc_calloc(b.c.requests, sizeof(*b.requests));
	check(b.requests != NULL, "request allocation");
	prepare(&b);
	must(xtc_loop_init(&loop));
	must(xtc_exec_class_create(loop, 100, 0, &cls));
	if (b.c.policy)
		must(xtc_sched_continuous_set(loop, 3));
	opts.capacity = b.c.active * 2;
	opts.max_active = b.c.active;
	opts.resource_capacity = b.c.active;
	opts.sched_class = cls;
	must(xtc_batch_create(loop, &opts, &b.batch));
	must(xtc_proc_spawn(loop, owner, &b, NULL, NULL));
	must(xtc_proc_spawn(loop, heartbeat, &b, NULL, NULL));
	must(xtc_loop_run(
	    loop)); /* Close -> driver exit -> destroy -> loop fini. */
	must(xtc_batch_get_stats(b.batch, &stats));
	check(stats.completed == b.c.requests && !stats.failed
	          && !stats.cancelled && !stats.queued && !stats.ready
	          && !stats.blocked && !stats.reserved_units
	          && stats.iterations == b.out.calls
	          && stats.total_members == b.members
	          && stats.rejections == b.out.rejections
	          && stats.max_vector <= b.c.active
	          && b.peak_resources <= b.c.active,
	      "drained engine accounting and bounds");
	for (i = 0; i < b.c.requests; i++) {
		r = &b.requests[i];
		check(r->terminals == 1 && r->rounds == r->length,
		      "fixed request work");
		check(b.start <= r->arrival && r->arrival <= r->admitted
		          && r->admitted <= r->first && r->first <= r->finished,
		      "timestamp ordering");
		for (j = 0; j < WIDTH; j++) {
			check(r->values[j] == r->expected[j],
			      "unsigned scalar oracle");
			b.out.checksum += r->values[j];
		}
		expected_work += (uint64_t)r->length * WIDTH;
		record(&admission, r->admitted - r->arrival);
		record(&first, r->first - r->arrival);
		record(&completion, r->finished - r->arrival);
	}
	check(b.out.work == expected_work && b.end > b.start
	          && b.heartbeat.count > 0,
	      "work conservation, measurement and heartbeat progress");
	b.out.completed = stats.completed;
	seconds = (double)(b.end - b.start) / 1e9;
	printf(
	    "mode=%s policy=%s workload=%s requests=%u active=%u rounds=%u spacing_ticks=%u\n",
	    b.c.continuous ? "continuous" : "static",
	    b.c.policy ? "continuous" : "default", workloads[b.c.workload],
	    b.c.requests, b.c.active, b.c.rounds, b.c.spacing);
	printf("seconds=%.6f completions/s=%.2f updates/s=%.2f updates=%" PRIu64
	       " checksum=%016" PRIx64 " oracle=OK\n",
	       seconds, b.c.requests / seconds, b.out.work / seconds,
	       b.out.work, b.out.checksum);
	printf("vector_calls=%" PRIu64
	       " calls/completion=%.6f occupancy_mean=%.3f"
	       " occupancy_fraction=%.6f max_vector=%" PRIu64
	       " peak_resources=%" PRIu64 " setup_entries=%" PRIu64
	       " rejections=%" PRIu64 " cancellation=not_used\n",
	       b.out.calls, (double)b.out.calls / b.c.requests,
	       (double)b.members / b.out.calls,
	       (double)b.members / b.out.calls / b.c.active, stats.max_vector,
	       b.peak_resources, b.out.calls * TABLE, b.out.rejections);
	print_hist("admission", &admission);
	print_hist("firststep", &first);
	print_hist("completion", &completion);
	print_hist("heartbeat_yield_delay", &b.heartbeat);
	must(xtc_batch_destroy(b.batch));
	must(xtc_loop_fini(loop));
	xtc_free(b.requests);
	check(fflush(stdout) == 0 && !ferror(stdout), "benchmark output");
	return b.out;
}

static unsigned
number(const char *s, unsigned max)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(s, &end, 10);
	check(*s >= '0' && *s <= '9' && !errno && !*end && n > 0 && n <= max,
	      "numeric option out of range");
	return (unsigned)n;
}

int
main(int argc, char **argv)
{
	struct config c = { 4096, 16, 64, 0, 1, 0, 1 };
	int i, w, test = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) {
			puts(
			    "usage: bench_continuous_batching [--self-test]\n"
			    "  --mode=static|continuous --policy=default|continuous\n"
			    "  --workload=lowload|saturated|overload|skew\n"
			    "  --requests=1..1000000 --active=1..1024 --rounds=1..4096\n"
			    "  --spacing=1..100000 (lowload owner ticks; default rounds*4)\n"
			    "Defaults: continuous/default/saturated, 4096 requests, active=16, rounds=64.\n"
			    "Fixed work; lowload arrivals follow logical owner ticks, not a wall-time rate.\n"
			    "Others arrive together: saturated/skew bounded feeder; overload retries full queue.\n"
			    "Latency: observed arrival to submit acceptance / first callback / completion.\n"
			    "Admission here means submit acceptance, NOT internal active-set admission.\n"
			    "Histogram log2 upper bounds (ns), exact max; occupancy is per-vector, not time-weighted.\n"
			    "Heartbeat measures yield-to-next-run delay, not periodic timer lateness.\n"
			    "Shared setup intentionally rebuilt per vector; no cancellation or duration mode.\n"
			    "Oracle and allocation excluded from timed window. Requires --enable-sched-policy.");
			check(fflush(stdout) == 0 && !ferror(stdout),
			      "help output");
			return 0;
		} else if (!strcmp(argv[i], "--self-test"))
			test = 1;
		else if (!strcmp(argv[i], "--mode=static"))
			c.continuous = 0;
		else if (!strcmp(argv[i], "--mode=continuous"))
			c.continuous = 1;
		else if (!strcmp(argv[i], "--policy=default"))
			c.policy = 0;
		else if (!strcmp(argv[i], "--policy=continuous"))
			c.policy = 1;
		else if (!strncmp(argv[i], "--workload=", 11)) {
			for (w = 0; w < 4 && strcmp(argv[i] + 11, workloads[w]);
			     w++)
				;
			check(w < 4, "unknown workload");
			c.workload = w;
		} else if (!strncmp(argv[i], "--requests=", 11))
			c.requests = number(argv[i] + 11, 1000000);
		else if (!strncmp(argv[i], "--active=", 9))
			c.active = number(argv[i] + 9, 1024);
		else if (!strncmp(argv[i], "--rounds=", 9))
			c.rounds = number(argv[i] + 9, 4096);
		else if (!strncmp(argv[i], "--spacing=", 10))
			c.spacing = number(argv[i] + 10, 100000);
		else
			check(0, "unknown option (try --help)");
	}
	if (test)
		self_test();
	else
		(void)run(&c);
	check(fflush(stdout) == 0 && !ferror(stdout), "final output");
	return 0;
}
