/*
 * diag.c - test-only: trial-by-trial diagnostics of the multi-lane decoder's
 * per-block choice (the race) against the base decoder.  Not for upstream.
 *
 * usage: diag LEVEL TRIALS FILE...
 *
 * For each file: compress it at LEVEL with the base compressor, then run
 * TRIALS interleaved trials of A (base) and B (multi-lane build with
 * -DMLSD_REPORT -DMLSD_REPORT_SYM=B_mlsd_report), first with a fresh B
 * decompressor per trial ("fresh"), then with one decompressor reused for all
 * trials ("reuse"), each with the output buffer at offsets 0 and 2048 within
 * a page.  Prints one line per trial: user-mode cycles (or ns if
 * there is no PMU) and instructions of A and B, B's speed relative to A, and
 * B's lane loop counters for that trial (blocks entered, skipped, races run,
 * races won by multi-lane, records executed by the lanes), then, for each
 * mode, B's races by cold/warm start and lanes/fastloop time ratio, if B
 * records them.  B's output is checked against the original every trial.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#  include <linux/perf_event.h>
#  include <sys/syscall.h>
#endif
#include "libdeflate.h"

#define DECL(p) \
	struct libdeflate_decompressor *p##_alloc_decompressor(void); \
	void p##_free_decompressor(struct libdeflate_decompressor *); \
	enum libdeflate_result p##_deflate_decompress( \
		struct libdeflate_decompressor *, const void *, size_t, \
		void *, size_t, size_t *);
DECL(A)
DECL(B)
extern unsigned long long B_mlsd_report[3][16];
/* races by [cold][ratio bucket][won], if B has them (see decompress_multilane.h) */
extern unsigned long long B_mlsd_racehist[2][16][2] __attribute__((weak));

/* print the races since 'h0' (a copy of B_mlsd_racehist) */
static void
print_races(const char *what, const unsigned long long h0[2][16][2])
{
	int c, b;

	if (!B_mlsd_racehist)
		return;
	for (c = 0; c < 2; c++) {
		unsigned long long n = 0, won = 0;
		char buf[512];
		int len = 0;

		for (b = 0; b < 16; b++) {
			unsigned long long l = B_mlsd_racehist[c][b][0] - h0[c][b][0];
			unsigned long long w = B_mlsd_racehist[c][b][1] - h0[c][b][1];

			n += l + w;
			won += w;
			if (l + w)
				len += snprintf(buf + len, sizeof(buf) - len,
						" %s%.1f:%llu/%llu",
						b == 0 ? "<" : (b == 15 ? ">=" : ""),
						b == 0 ? 0.6 : 0.5 + 0.1 * b, w, l + w);
		}
		printf("  races %s %s: %llu, lanes won %llu (ratio bucket: won/all)%s\n",
		       what, c ? "cold" : "warm", n, won, len ? buf : "");
	}
}

static int fd_cyc = -1, fd_ins = -1;

static int __attribute__((unused))
perf_open(uint64_t config)
{
#ifdef __linux__
	struct perf_event_attr a;

	memset(&a, 0, sizeof(a));
	a.type = PERF_TYPE_HARDWARE;
	a.size = sizeof(a);
	a.config = config;
	a.exclude_kernel = 1;
	a.exclude_hv = 1;
	return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
#else
	(void)config;
	return -1;
#endif
}

static uint64_t
rd(int fd)
{
	uint64_t v = 0;

	if (fd < 0 || read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
		return 0;
	return v;
}

/* B's lane loop counters, summed over the kernels (scalar, emulated, AVX-512) */
static void
counters(unsigned long long c[16])
{
	int k, i;

	for (i = 0; i < 16; i++)
		c[i] = 0;
	for (k = 0; k < 3; k++)
		for (i = 0; i < 16; i++)
			c[i] += B_mlsd_report[k][i];
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* time (cycles, or ns without a PMU) and instructions of one decompression */
static void
run1(int b, struct libdeflate_decompressor *d, const void *c, size_t clen,
     void *out, size_t n, uint64_t *t, uint64_t *ins)
{
	uint64_t c0, c1, i0, i1, n0, n1;
	enum libdeflate_result r;

	i0 = rd(fd_ins);
	c0 = rd(fd_cyc);
	n0 = now_ns();
	r = b ? B_deflate_decompress(d, c, clen, out, n, NULL) :
		A_deflate_decompress(d, c, clen, out, n, NULL);
	n1 = now_ns();
	c1 = rd(fd_cyc);
	i1 = rd(fd_ins);
	if (r != LIBDEFLATE_SUCCESS) {
		fprintf(stderr, "decompression failed (%c)\n", b ? 'B' : 'A');
		exit(1);
	}
	*t = fd_cyc >= 0 ? c1 - c0 : n1 - n0;
	*ins = i1 - i0;
}

int
main(int argc, char **argv)
{
	int level, trials, f, stack_mark = 0;
	struct libdeflate_compressor *comp;

	if (argc < 4) {
		fprintf(stderr, "usage: diag LEVEL TRIALS FILE...\n");
		return 2;
	}
	level = atoi(argv[1]);
	trials = atoi(argv[2]);
#ifdef __linux__
	fd_cyc = perf_open(PERF_COUNT_HW_CPU_CYCLES);
	fd_ins = perf_open(PERF_COUNT_HW_INSTRUCTIONS);
#endif
	comp = libdeflate_alloc_compressor(level);
	printf("# pid %d, stack %% 4096 = %lu, timing %s\n", (int)getpid(),
	       (unsigned long)((uintptr_t)&stack_mark % 4096),
	       fd_cyc >= 0 ? "user-mode cycles" : "ns");
	for (f = 3; f < argc; f++) {
		FILE *fp = fopen(argv[f], "rb");
		const char *name = strrchr(argv[f], '/');
		unsigned char *data, *cbuf, *obuf;
		size_t n, cbound, clen;
		int mode, oi, t;

		name = name ? name + 1 : argv[f];
		if (!fp) {
			perror(argv[f]);
			return 1;
		}
		fseek(fp, 0, SEEK_END);
		n = (size_t)ftell(fp);
		fseek(fp, 0, SEEK_SET);
		data = malloc(n);
		if (!data || fread(data, 1, n, fp) != n) {
			fprintf(stderr, "%s: read error\n", argv[f]);
			return 1;
		}
		fclose(fp);
		cbound = libdeflate_deflate_compress_bound(comp, n);
		cbuf = malloc(cbound);
		obuf = malloc(n + 8192);
		clen = libdeflate_deflate_compress(comp, data, n, cbuf, cbound);
		if (!cbuf || !obuf || !clen) {
			fprintf(stderr, "%s: compression failed\n", name);
			return 1;
		}
		printf("# %s L%d: %zu -> %zu bytes, input %% 4096 = %lu\n", name,
		       level, n, clen, (unsigned long)((uintptr_t)cbuf % 4096));
		for (mode = 0; mode < 2; mode++) {
			struct libdeflate_decompressor *da = A_alloc_decompressor();
			struct libdeflate_decompressor *db = NULL;
			unsigned long long h0[2][16][2];

			if (B_mlsd_racehist)
				memcpy(h0, B_mlsd_racehist, sizeof(h0));

			if (mode == 1)
				db = B_alloc_decompressor();
			for (oi = 0; oi < 4; oi += 2) {
				/* output at offset oi * 1024 within a page */
				unsigned char *out = obuf + ((4096 -
					(uintptr_t)obuf % 4096) % 4096) % 4096;

				if ((size_t)(out - obuf) + oi * 1024 + n >
				    n + 8192)
					out = obuf;
				out += oi * 1024;
				for (t = 0; t < trials; t++) {
					unsigned long long c0[16], c1[16];
					uint64_t ta, ia, tb, ib;
					int k;

					if (mode == 0)
						db = B_alloc_decompressor();
					run1(0, da, cbuf, clen, out, n, &ta, &ia);
					counters(c0);
					run1(1, db, cbuf, clen, out, n, &tb, &ib);
					counters(c1);
					if (memcmp(out, data, n) != 0) {
						printf("%s: B output MISMATCH\n",
						       name);
						return 1;
					}
					for (k = 0; k < 16; k++)
						c1[k] -= c0[k];
					printf("%s L%d %s ofs %4lu t %d: A %.2fM %.2fMi | B %.2fM %.2fMi | speed %+.1f%% ins %+.1f%% | entered %llu skipped %llu judged %llu races %llu won %llu recs %llu\n",
					       name, level, mode ? "reuse" : "fresh",
					       (unsigned long)((uintptr_t)out % 4096),
					       t, ta / 1e6, ia / 1e6, tb / 1e6,
					       ib / 1e6,
					       ((double)ta / tb - 1) * 100,
					       ia ? ((double)ib / ia - 1) * 100 : 0.0,
					       c1[0], c1[7], c1[11], c1[14], c1[15],
					       c1[3]);
					if (mode == 0) {
						B_free_decompressor(db);
						db = NULL;
					}
				}
			}
			if (db)
				B_free_decompressor(db);
			A_free_decompressor(da);
			{
				char what[64];

				snprintf(what, sizeof(what), "%s L%d %s", name,
					 level, mode ? "reuse" : "fresh");
				print_races(what, h0);
			}
		}
		free(data);
		free(cbuf);
		free(obuf);
	}
	libdeflate_free_compressor(comp);
	return 0;
}
