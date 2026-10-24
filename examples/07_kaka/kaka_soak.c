/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/07_kaka/kaka_soak.c -- concurrent network + storage soak.
 *
 *   kaka_soak HOST PORT TOPICS PRODUCERS CONSUMERS SECS VALUE_BYTES BATCH
 *
 * kaka_load produces, then consumes; nothing overlaps.  This one runs
 * PRODUCERS producer threads and CONSUMERS consumer threads against
 * TOPICS topics (partition 0 each) at the same time for SECS seconds,
 * so the broker is writing and fdatasync'ing segments, reading cold
 * segments back through xtc_aio, and moving both over TCP, all at once,
 * on the same loops.  Each producer owns one topic stream (producer i
 * writes topic i % TOPICS) and tags every record with (producer, seq).
 * Consumers tail their topic (consumer j reads topic j % TOPICS) while
 * it grows; FETCH returns only acknowledged records.
 *
 * The topics must be new (an empty data directory, as test_kaka_soak.sh
 * makes): a consumer starts at offset 0 and expects the first record of
 * each producer to be its sequence 0.
 *
 * Checked, and any violation fails the run (exit 1):
 *   - offsets a consumer sees are dense and increasing (no gap, no
 *     repeat, no reorder);
 *   - every record's value is the pattern its key says it should be
 *     (no torn or misdirected bytes);
 *   - per (producer, topic), sequence numbers arrive in order;
 *   - after the producers stop, every consumer catches up to exactly
 *     the number of records that topic's producers had acknowledged.
 *
 * Prints one line:
 *   produced=N acked_MiB_s=X consumed=M consumed_MiB_s=Y errors=0
 * A plain blocking client (no libxtc): it measures and checks the broker.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXT     64
#define MAXP     64

static struct sockaddr_in g_sa;
static int g_topics, g_prod, g_cons, g_vlen, g_batch;
static double g_end;
static _Atomic int g_prod_done, g_err;
static _Atomic uint64_t g_produced, g_consumed;
static _Atomic uint64_t g_acked[MAXT];     /* records acked per topic */

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t
get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t
get64(const uint8_t *p)
{
	return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
fail(const char *what, unsigned a, unsigned long long b)
{
	if (atomic_fetch_add(&g_err, 1) < 8)
		fprintf(stderr, "FAIL: %s (%u, %llu)\n", what, a, b);
}

static int
write_all(int fd, const uint8_t *p, size_t n)
{
	ssize_t w;

	while (n > 0) {
		if ((w = send(fd, p, n, MSG_NOSIGNAL)) <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int
read_all(int fd, uint8_t *p, size_t n)
{
	ssize_t r;

	while (n > 0) {
		if ((r = recv(fd, p, n, 0)) <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static int
read_frame(int fd, uint8_t **buf, size_t *cap, size_t *len)
{
	uint8_t hdr[5], *nb;
	size_t n;

	if (read_all(fd, hdr, 5) != 0)
		return -1;
	n = get32(hdr) - 1;
	if (n > *cap) {
		if ((nb = realloc(*buf, n)) == NULL)
			return -1;
		*buf = nb;
		*cap = n;
	}
	if (read_all(fd, *buf, n) != 0)
		return -1;
	*len = n;
	return hdr[4];
}

static int
dial(void)
{
	int fd, one = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&g_sa, sizeof g_sa) != 0) {
		close(fd);
		return -1;
	}
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

/* Value byte k of record (producer, seq): checkable by any reader. */
static uint8_t
pat(uint32_t prod, uint32_t seq, uint32_t k)
{
	return (uint8_t)(prod * 131u + seq * 7u + k);
}

static void
topic_name(char *out, size_t n, int t)
{
	snprintf(out, n, "soak%d", t);
}

static void *
producer(void *arg)
{
	int id = (int)(intptr_t)arg, fd, t = id % g_topics;
	size_t tl, at, cap = 0, len, pcap;
	uint8_t *p, *rbuf = NULL;
	uint32_t seq = 0, k, j;
	char topic[16];

	topic_name(topic, sizeof topic, t);
	tl = strlen(topic);
	pcap = 5 + 2 + tl + 8 + (size_t)g_batch * (16 + 8 + g_vlen);
	if ((p = malloc(pcap)) == NULL || (fd = dial()) < 0) {
		fail("producer connect", (unsigned)id, 0);
		free(p);
		return NULL;
	}
	while (now_s() < g_end) {
		at = 5;
		p[at++] = (uint8_t)(tl >> 8);
		p[at++] = (uint8_t)tl;
		memcpy(p + at, topic, tl);
		at += tl;
		put32(p + at, 0); at += 4;
		put32(p + at, (uint32_t)g_batch); at += 4;
		for (j = 0; j < (uint32_t)g_batch; j++, seq++) {
			put32(p + at, 8); put32(p + at + 4, (uint32_t)g_vlen);
			at += 8;
			put32(p + at, (uint32_t)id); put32(p + at + 4, seq);
			at += 8;
			for (k = 0; k < (uint32_t)g_vlen; k++)
				p[at + k] = pat((uint32_t)id, seq, k);
			at += (size_t)g_vlen;
		}
		put32(p, (uint32_t)(at - 4));
		p[4] = 0x01;
		if (write_all(fd, p, at) != 0 ||
		    read_frame(fd, &rbuf, &cap, &len) != 0x81) {
			fail("produce", (unsigned)id, seq);
			break;
		}
		atomic_fetch_add(&g_acked[t], (uint64_t)g_batch);
		atomic_fetch_add(&g_produced, (uint64_t)g_batch);
	}
	close(fd);
	free(p);
	free(rbuf);
	return NULL;
}

static void *
consumer(void *arg)
{
	int id = (int)(intptr_t)arg, fd, t = id % g_topics, typ;
	uint32_t next_seq[MAXP], prod, seq, kl, vl, n, k, j;
	size_t tl, at, q, cap = 0, len;
	uint8_t req[128], *rbuf = NULL;
	uint64_t off = 0, got = 0;
	char topic[16];

	memset(next_seq, 0, sizeof next_seq);
	topic_name(topic, sizeof topic, t);
	tl = strlen(topic);
	if ((fd = dial()) < 0) {
		fail("consumer connect", (unsigned)id, 0);
		return NULL;
	}
	for (;;) {
		/* Done when the producers have stopped and this consumer has
		 * read every record they had acknowledged for its topic. */
		if (atomic_load(&g_prod_done) &&
		    off >= atomic_load(&g_acked[t]))
			break;
		at = 5;
		req[at++] = (uint8_t)(tl >> 8);
		req[at++] = (uint8_t)tl;
		memcpy(req + at, topic, tl);
		at += tl;
		put32(req + at, 0); at += 4;
		put32(req + at, (uint32_t)(off >> 32));
		put32(req + at + 4, (uint32_t)off); at += 8;
		put32(req + at, 1u << 20); at += 4;
		put32(req, (uint32_t)(at - 4));
		req[4] = 0x02;
		if (write_all(fd, req, at) != 0) {
			fail("fetch send", (unsigned)id, off);
			break;
		}
		typ = read_frame(fd, &rbuf, &cap, &len);
		if (typ != 0x82 || len < 12) {
			if (typ == 0xff && len >= 2)
				fprintf(stderr, "consumer %d: ERROR code %u: "
				    "%.*s\n", id, (unsigned)((rbuf[0] << 8) |
				    rbuf[1]), (int)(len > 2 ? len - 2 : 0),
				    (const char *)rbuf + 2);
			else
				fprintf(stderr, "consumer %d: frame type %d "
				    "len %zu\n", id, typ, len);
			fail("fetch", (unsigned)id, off);
			break;
		}
		n = get32(rbuf + 8);
		if (n == 0) {
			usleep(500);
			continue;
		}
		for (q = 12, j = 0; j < n; j++) {
			if (q + 16 > len || get64(rbuf + q) != off) {
				fail("offset gap or reorder", (unsigned)id, off);
				goto out;
			}
			kl = get32(rbuf + q + 8);
			vl = get32(rbuf + q + 12);
			q += 16;
			if (kl != 8 || vl != (uint32_t)g_vlen || q + kl + vl > len) {
				fail("record shape", (unsigned)id, off);
				goto out;
			}
			prod = get32(rbuf + q);
			seq = get32(rbuf + q + 4);
			if (prod >= MAXP || (int)(prod % (uint32_t)g_topics) != t) {
				fail("misdirected record", (unsigned)id, off);
				goto out;
			}
			if (seq != next_seq[prod]) {
				fail("producer sequence", prod, seq);
				goto out;
			}
			next_seq[prod] = seq + 1;
			for (k = 0; k < vl; k++)
				if (rbuf[q + kl + k] != pat(prod, seq, k)) {
					fail("value bytes", (unsigned)id, off);
					goto out;
				}
			q += kl + vl;
			off++;
			got++;
		}
	}
out:
	atomic_fetch_add(&g_consumed, got);
	close(fd);
	free(rbuf);
	return NULL;
}

int
main(int argc, char **argv)
{
	pthread_t pt[MAXP], ct[MAXP];
	double t0, t1, t2, rec_mib;
	int secs, i;
	uint64_t want;

	if (argc < 9) {
		fprintf(stderr, "usage: %s HOST PORT TOPICS PRODUCERS "
		    "CONSUMERS SECS VALUE_BYTES BATCH\n", argv[0]);
		return 2;
	}
	memset(&g_sa, 0, sizeof g_sa);
	g_sa.sin_family = AF_INET;
	g_sa.sin_port = htons((uint16_t)atoi(argv[2]));
	if (inet_pton(AF_INET, argv[1], &g_sa.sin_addr) != 1)
		return 2;
	g_topics = atoi(argv[3]);
	g_prod = atoi(argv[4]);
	g_cons = atoi(argv[5]);
	secs = atoi(argv[6]);
	g_vlen = atoi(argv[7]);
	g_batch = atoi(argv[8]);
	if (g_topics < 1 || g_topics > MAXT || g_prod < g_topics ||
	    g_prod > MAXP || g_cons < 1 || g_cons > MAXP || secs < 1 ||
	    g_vlen < 1 || g_batch < 1)
		return 2;
	t0 = now_s();
	g_end = t0 + secs;
	for (i = 0; i < g_cons; i++)
		pthread_create(&ct[i], NULL, consumer, (void *)(intptr_t)i);
	for (i = 0; i < g_prod; i++)
		pthread_create(&pt[i], NULL, producer, (void *)(intptr_t)i);
	for (i = 0; i < g_prod; i++)
		pthread_join(pt[i], NULL);
	t1 = now_s();
	atomic_store(&g_prod_done, 1);
	for (i = 0; i < g_cons; i++)
		pthread_join(ct[i], NULL);
	t2 = now_s();
	/* every consumer read its whole topic: sum over consumers */
	want = 0;
	for (i = 0; i < g_cons; i++)
		want += atomic_load(&g_acked[i % g_topics]);
	if (atomic_load(&g_err) == 0 && atomic_load(&g_consumed) != want)
		fail("consumers short of the acked total",
		    (unsigned)atomic_load(&g_consumed), want);
	rec_mib = (double)(16 + 8 + g_vlen) / (1024.0 * 1024.0);
	printf("produced=%llu acked_MiB_s=%.1f consumed=%llu "
	    "consumed_MiB_s=%.1f drain_s=%.2f errors=%d\n",
	    (unsigned long long)atomic_load(&g_produced),
	    (double)atomic_load(&g_produced) * rec_mib / (t1 - t0),
	    (unsigned long long)atomic_load(&g_consumed),
	    (double)atomic_load(&g_consumed) * rec_mib / (t2 - t0),
	    t2 - t1, atomic_load(&g_err));
	return atomic_load(&g_err) ? 1 : 0;
}
