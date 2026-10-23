/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/kaka_load.c -- a small produce + consume load driver.
 *
 *   kaka_load HOST PORT TOPIC NREC VALUE_BYTES BATCH [FETCH_BYTES]
 *
 * Produces NREC records of VALUE_BYTES (key 8 bytes) in PRODUCE frames of
 * BATCH records to TOPIC partition 0, waiting for each ack, then FETCHes
 * from offset 0 until all NREC records have come back (FETCH_BYTES per
 * request, default 1 MiB), checking every record's offset.  Prints:
 *   produce_s produce_MiB_s consume_s consume_MiB_s
 * A plain blocking client (no libxtc): it measures the broker.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

static int
write_all(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int
read_all(int fd, uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t r = recv(fd, p, n, 0);
		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

/* Read one response frame into *buf (grown as needed); returns type. */
static int
read_frame(int fd, uint8_t **buf, size_t *cap, size_t *len)
{
	uint8_t hdr[5];
	size_t n;
	if (read_all(fd, hdr, 5) != 0)
		return -1;
	n = get32(hdr) - 1;
	if (n > *cap) {
		uint8_t *nb = realloc(*buf, n);
		if (nb == NULL)
			return -1;
		*buf = nb;
		*cap = n;
	}
	if (read_all(fd, *buf, n) != 0)
		return -1;
	*len = n;
	return hdr[4];
}

int
main(int argc, char **argv)
{
	struct sockaddr_in sa;
	const char *topic;
	uint32_t nrec, vlen, batch, fbytes, i, k;
	size_t tl, rec, cap = 0, len, pcap;
	uint8_t *p, *rbuf = NULL;
	uint64_t off = 0;
	double t0, t1, t2, mib;
	int fd, one = 1;

	if (argc < 7) {
		fprintf(stderr, "usage: %s HOST PORT TOPIC NREC VALUE_BYTES "
		    "BATCH [FETCH_BYTES]\n", argv[0]);
		return 2;
	}
	topic = argv[3];
	tl = strlen(topic);
	nrec = (uint32_t)strtoul(argv[4], NULL, 10);
	vlen = (uint32_t)strtoul(argv[5], NULL, 10);
	batch = (uint32_t)strtoul(argv[6], NULL, 10);
	fbytes = argc > 7 ? (uint32_t)strtoul(argv[7], NULL, 10) : 1u << 20;
	rec = 8 + 8 + vlen;
	pcap = 5 + 2 + tl + 4 + 4 + (size_t)batch * rec;
	if ((p = malloc(pcap)) == NULL)
		return 1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)atoi(argv[2]));
	if (inet_pton(AF_INET, argv[1], &sa.sin_addr) != 1)
		return 2;
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
	    connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		perror("connect");
		return 1;
	}
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	t0 = now_s();
	for (i = 0; i < nrec; i += batch) {
		uint32_t nb = nrec - i < batch ? nrec - i : batch;
		size_t at = 5;
		p[at++] = (uint8_t)(tl >> 8);
		p[at++] = (uint8_t)tl;
		memcpy(p + at, topic, tl);
		at += tl;
		put32(p + at, 0); at += 4;
		put32(p + at, nb); at += 4;
		for (k = 0; k < nb; k++) {
			put32(p + at, 8); put32(p + at + 4, vlen); at += 8;
			put32(p + at, i + k); put32(p + at + 4, 0); at += 8;
			memset(p + at, (int)((i + k) & 0xff), vlen); at += vlen;
		}
		put32(p, (uint32_t)(at - 4));
		p[4] = 0x01;
		if (write_all(fd, p, at) != 0 ||
		    read_frame(fd, &rbuf, &cap, &len) != 0x81) {
			fprintf(stderr, "produce failed at %u\n", i);
			return 1;
		}
	}
	t1 = now_s();
	while (off < nrec) {
		size_t at = 5, q;
		uint32_t n;
		p[at++] = (uint8_t)(tl >> 8);
		p[at++] = (uint8_t)tl;
		memcpy(p + at, topic, tl);
		at += tl;
		put32(p + at, 0); at += 4;
		put32(p + at, (uint32_t)(off >> 32));
		put32(p + at + 4, (uint32_t)off); at += 8;
		put32(p + at, fbytes); at += 4;
		put32(p, (uint32_t)(at - 4));
		p[4] = 0x02;
		if (write_all(fd, p, at) != 0 ||
		    read_frame(fd, &rbuf, &cap, &len) != 0x82 || len < 12) {
			fprintf(stderr, "fetch failed at %llu\n",
			    (unsigned long long)off);
			return 1;
		}
		n = get32(rbuf + 8);
		for (q = 12, k = 0; k < n; k++) {
			if (get64(rbuf + q) != off) {
				fprintf(stderr, "bad offset %llu\n",
				    (unsigned long long)off);
				return 1;
			}
			q += 16 + get32(rbuf + q + 8) + get32(rbuf + q + 12);
			off++;
		}
		if (n == 0) {
			fprintf(stderr, "empty fetch at %llu\n",
			    (unsigned long long)off);
			return 1;
		}
	}
	t2 = now_s();
	mib = (double)nrec * vlen / (1024.0 * 1024.0);
	printf("%.3f %.1f %.3f %.1f\n", t1 - t0, mib / (t1 - t0), t2 - t1,
	    mib / (t2 - t1));
	close(fd);
	free(p);
	free(rbuf);
	return 0;
}
