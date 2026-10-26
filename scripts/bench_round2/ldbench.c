/*
 * ldbench.c - low-noise A/B benchmark harness for libdeflate.
 *
 * usage: ldbench [-l LEVEL] [-s CHUNK] [-r REPS] [-m c|d|cd] FILE...
 *
 * Compresses FILE in chunks of CHUNK bytes (0 = whole file) at LEVEL, then
 * times REPS repetitions of compression and/or decompression.  For each it
 * reports, per rep, wall time AND user-mode CPU cycles (via perf_event_open),
 * and prints the minimum and median.  Cycle counts are insensitive to clock
 * frequency changes and to time spent descheduled, so they are much less noisy
 * on a busy machine than wall-clock time.
 *
 * Output is a single machine-parseable line per file:
 *   name level ratio% c_cpb_min c_cpb_med c_mbs_med d_cpb_min d_cpb_med d_mbs_med csize
 * (cpb = cycles per uncompressed byte)
 */
#define _GNU_SOURCE
#include <libdeflate.h>
#ifdef __linux__
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * The "cycles" counter: user-mode CPU cycles on Linux if perf_event_open
 * works, otherwise (e.g. macOS) monotonic nanoseconds.  Either way the A/B
 * scripts only compare ratios.
 */
static int cyc_fd = -1;

static uint64_t ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void cyc_init(void)
{
#ifdef __linux__
	struct perf_event_attr pe;

	memset(&pe, 0, sizeof(pe));
	pe.type = PERF_TYPE_HARDWARE;
	pe.size = sizeof(pe);
	pe.config = PERF_COUNT_HW_CPU_CYCLES;
	pe.exclude_kernel = 1;
	pe.exclude_hv = 1;
	cyc_fd = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
#endif
	if (cyc_fd < 0 && getenv("LDBENCH_QUIET") == NULL)
		fprintf(stderr, "note: no cycle counter; reporting nanoseconds\n");
}

static uint64_t cyc_read(void)
{
	uint64_t v = 0;

	if (cyc_fd < 0)
		return ns_now();
	if (read(cyc_fd, &v, sizeof(v)) != sizeof(v))
		v = 0;
	return v;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	int level = 6, reps = 15, opt, print_hash = 0;
	size_t chunk = 0;
	const char *mode = "d";

	while ((opt = getopt(argc, argv, "l:s:r:m:H")) != -1) {
		switch (opt) {
		case 'l': level = atoi(optarg); break;
		case 's': chunk = strtoull(optarg, NULL, 0); break;
		case 'r': reps = atoi(optarg); break;
		case 'm': mode = optarg; break;
		case 'H': print_hash = 1; break;
		default: return 2;
		}
	}
	cyc_init();
	for (int fi = optind; fi < argc; fi++) {
		FILE *f = fopen(argv[fi], "rb");
		if (!f) { perror(argv[fi]); return 1; }
		fseek(f, 0, SEEK_END);
		size_t n = ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char *in = malloc(n + 1);
		if (fread(in, 1, n, f) != n) { perror("read"); return 1; }
		fclose(f);
		size_t cs = chunk ? chunk : n;
		size_t nchunks = (n + cs - 1) / cs;
		struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
		struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
		size_t bound = libdeflate_deflate_compress_bound(c, cs);
		unsigned char *cbuf = malloc(nchunks * bound);
		size_t *clen = malloc(nchunks * sizeof(size_t));
		unsigned char *out = malloc(n + 64);
		size_t ctotal = 0;
		double *ct = calloc(reps, sizeof(double));
		double *cc = calloc(reps, sizeof(double));
		double *dt = calloc(reps, sizeof(double));
		double *dc = calloc(reps, sizeof(double));
		int do_c = strchr(mode, 'c') != NULL;
		int do_d = strchr(mode, 'd') != NULL;

		memset(out, 0, n + 64); /* fault in the output buffer */
		for (int r = 0; r < (do_c ? reps : 1); r++) {
			double t0 = now();
			uint64_t c0 = cyc_read();
			ctotal = 0;
			for (size_t i = 0; i < nchunks; i++) {
				size_t len = (i == nchunks - 1) ? n - i * cs : cs;
				clen[i] = libdeflate_deflate_compress(c, in + i * cs, len,
								      cbuf + i * bound, bound);
				if (clen[i] == 0) { fprintf(stderr, "compress failed\n"); return 1; }
				ctotal += clen[i];
			}
			cc[r] = (double)(cyc_read() - c0) / n;
			ct[r] = now() - t0;
		}
		for (int r = 0; r < (do_d ? reps : 0); r++) {
			double t0 = now();
			uint64_t c0 = cyc_read();
			for (size_t i = 0; i < nchunks; i++) {
				size_t len = (i == nchunks - 1) ? n - i * cs : cs;
				enum libdeflate_result res =
					libdeflate_deflate_decompress(d, cbuf + i * bound, clen[i],
								      out + i * cs, len, NULL);
				if (res != LIBDEFLATE_SUCCESS) { fprintf(stderr, "decompress failed: %d\n", res); return 1; }
			}
			dc[r] = (double)(cyc_read() - c0) / n;
			dt[r] = now() - t0;
			if (r == 0 && memcmp(in, out, n) != 0) { fprintf(stderr, "MISMATCH\n"); return 1; }
		}
		const char *name = strrchr(argv[fi], '/');
		name = name ? name + 1 : argv[fi];
		printf("%s %d %.3f", name, level, 100.0 * ctotal / n);
		if (do_c) {
			qsort(ct, reps, sizeof(double), cmpd);
			qsort(cc, reps, sizeof(double), cmpd);
			printf(" %.4f %.4f %.1f", cc[0], cc[reps / 2], n / ct[reps / 2] / 1e6);
		} else {
			printf(" - - -");
		}
		if (do_d) {
			qsort(dt, reps, sizeof(double), cmpd);
			qsort(dc, reps, sizeof(double), cmpd);
			printf(" %.4f %.4f %.1f", dc[0], dc[reps / 2], n / dt[reps / 2] / 1e6);
		} else {
			printf(" - - -");
		}
		if (print_hash) {
			unsigned long long h = 1469598103934665603ULL;
			for (size_t i = 0; i < nchunks; i++)
				for (size_t j = 0; j < clen[i]; j++)
					h = (h ^ cbuf[i * bound + j]) * 1099511628211ULL;
			printf(" %zu %016llx\n", ctotal, h);
		} else {
			printf(" %zu\n", ctotal);
		}
		fflush(stdout);
		libdeflate_free_compressor(c);
		libdeflate_free_decompressor(d);
		free(in); free(cbuf); free(clen); free(out);
		free(ct); free(cc); free(dt); free(dc);
	}
	return 0;
}
