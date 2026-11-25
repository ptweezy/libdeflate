/*
 * bench.c - test-only: minimal A/B benchmark harness for libdeflate.
 *
 * usage: bench [-l L1,L2,..] [-c CHUNK] [-n TRIALS] [-t MIN_MS] [-D] [-C] FILE...
 *
 * For each file and level, prints one line:
 *   name level in_size out_size out_hash comp_time decomp_time comp_instr decomp_instr
 * The times are the best (minimum) of one full pass.  They are nanoseconds, or
 * user-mode CPU cycles if the environment variable BENCH_PMU is set (Linux).
 * The instruction counts are 0 unless BENCH_PMU is set.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "libdeflate.h"

#ifdef _WIN32
#include <windows.h>
static uint64_t now_ns(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER c;
	if (!f.QuadPart)
		QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
#endif

#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
static int pmu_fd[2] = { -1, -1 };
static int use_pmu;
static void pmu_init(void)
{
	static const uint64_t cfg[2] = { PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS };
	int i;
	if (!getenv("BENCH_PMU"))
		return;
	for (i = 0; i < 2; i++) {
		struct perf_event_attr a;
		memset(&a, 0, sizeof(a));
		a.type = PERF_TYPE_HARDWARE; a.size = sizeof(a); a.config = cfg[i];
		a.exclude_kernel = 1; a.exclude_hv = 1;
		pmu_fd[i] = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
		if (pmu_fd[i] < 0) { perror("perf_event_open"); exit(1); }
	}
	use_pmu = 1;
}
static uint64_t pmu_read(int i)
{
	uint64_t v = 0;
	if (read(pmu_fd[i], &v, 8) != 8) v = 0;
	return v;
}
static uint64_t tick(void) { return use_pmu ? pmu_read(0) : now_ns(); }
static uint64_t instr(void) { return use_pmu ? pmu_read(1) : 0; }
#else
static int use_pmu;
static void pmu_init(void) {}
static uint64_t tick(void) { return now_ns(); }
static uint64_t instr(void) { return 0; }
#endif

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) { fprintf(stderr, "oom\n"); exit(1); }
	return p;
}

int main(int argc, char **argv)
{
	int levels[16], nlevels = 0;
	size_t chunk = 0;
	int trials = 5;
	double min_ms = 0;
	int do_decomp = 1, do_comp = 1;
	int i, a = 1;
	int fmt = 0; /* 0 deflate, 1 zlib, 2 gzip */

	pmu_init();
	levels[nlevels++] = 6;
	for (; a < argc && argv[a][0] == '-'; a++) {
		if (!strcmp(argv[a], "-l")) {
			char *s = argv[++a];
			nlevels = 0;
			while (*s) {
				levels[nlevels++] = (int)strtol(s, &s, 10);
				if (*s == ',') s++;
			}
		} else if (!strcmp(argv[a], "-c")) {
			chunk = (size_t)strtoull(argv[++a], NULL, 10);
		} else if (!strcmp(argv[a], "-n")) {
			trials = atoi(argv[++a]);
		} else if (!strcmp(argv[a], "-t")) {
			min_ms = atof(argv[++a]);
		} else if (!strcmp(argv[a], "-D")) {
			do_decomp = 0;
		} else if (!strcmp(argv[a], "-C")) {
			do_comp = 0;
		} else if (!strcmp(argv[a], "-z")) {
			fmt = 1;
		} else if (!strcmp(argv[a], "-g")) {
			fmt = 2;
		} else {
			fprintf(stderr, "bad option %s\n", argv[a]);
			return 2;
		}
	}

	for (; a < argc; a++) {
		const char *name = argv[a];
		const char *base = strrchr(name, '/');
		FILE *f = fopen(name, "rb");
		uint8_t *in, *out, *dec;
		size_t in_size, out_cap, nchunks, c;
		size_t *csizes;
		struct libdeflate_decompressor *d;

		base = base ? base + 1 : name;
		if (!f) { perror(name); return 1; }
		fseek(f, 0, SEEK_END);
		in_size = (size_t)ftell(f);
		fseek(f, 0, SEEK_SET);
		in = xmalloc(in_size);
		if (fread(in, 1, in_size, f) != in_size) { perror("read"); return 1; }
		fclose(f);

		{
			size_t ck = chunk ? chunk : in_size;
			nchunks = in_size ? (in_size + ck - 1) / ck : 0;
		}
		csizes = xmalloc(nchunks * sizeof(*csizes));
		dec = xmalloc(in_size);
		d = libdeflate_alloc_decompressor();

		for (i = 0; i < nlevels; i++) {
			struct libdeflate_compressor *cmp =
				libdeflate_alloc_compressor(levels[i]);
			size_t ck = chunk ? chunk : in_size;
			size_t bound, total = 0;
			uint64_t best_c = UINT64_MAX, best_d = UINT64_MAX;
			uint64_t ins_c = UINT64_MAX, ins_d = UINT64_MAX;
			uint64_t t0, t1, i0, spent;
			uint32_t hash = 0;
			int t;

			if (!cmp) { fprintf(stderr, "alloc fail\n"); return 1; }
			bound = fmt == 0 ? libdeflate_deflate_compress_bound(cmp, ck) :
				fmt == 1 ? libdeflate_zlib_compress_bound(cmp, ck) :
					   libdeflate_gzip_compress_bound(cmp, ck);
			out_cap = bound * nchunks;
			out = xmalloc(out_cap);

			spent = 0;
			for (t = 0; t < (do_comp ? trials : 1) ||
				    (do_comp && !use_pmu && spent < (uint64_t)(min_ms * 1e6)); t++) {
				size_t off = 0, opos = 0;

				i0 = instr();
				t0 = tick();
				for (c = 0; c < nchunks; c++) {
					size_t n = in_size - off < ck ? in_size - off : ck;
					size_t r = fmt == 0 ?
						libdeflate_deflate_compress(cmp, in + off, n, out + opos, bound) :
						fmt == 1 ?
						libdeflate_zlib_compress(cmp, in + off, n, out + opos, bound) :
						libdeflate_gzip_compress(cmp, in + off, n, out + opos, bound);
					if (!r) { fprintf(stderr, "compress fail\n"); return 1; }
					csizes[c] = r;
					off += n;
					opos += r;
				}
				t1 = tick();
				i0 = instr() - i0;
				if (i0 < ins_c)
					ins_c = i0;
				if (t1 - t0 < best_c)
					best_c = t1 - t0;
				spent += t1 - t0;
				total = opos;
			}
			hash = libdeflate_crc32(0, out, total);
			for (c = 0; c < nchunks; c++) {
				uint64_t v = csizes[c];
				hash = libdeflate_crc32(hash, &v, sizeof(v));
			}

			spent = 0;
			for (t = 0; do_decomp && (t < trials ||
			     (!use_pmu && spent < (uint64_t)(min_ms * 1e6))); t++) {
				size_t off = 0, opos = 0;

				i0 = instr();
				t0 = tick();
				for (c = 0; c < nchunks; c++) {
					size_t n = in_size - off < ck ? in_size - off : ck;
					enum libdeflate_result r = fmt == 0 ?
						libdeflate_deflate_decompress(d, out + opos, csizes[c], dec + off, n, NULL) :
						fmt == 1 ?
						libdeflate_zlib_decompress(d, out + opos, csizes[c], dec + off, n, NULL) :
						libdeflate_gzip_decompress(d, out + opos, csizes[c], dec + off, n, NULL);
					if (r != LIBDEFLATE_SUCCESS) { fprintf(stderr, "decompress fail %d\n", r); return 1; }
					off += n;
					opos += csizes[c];
				}
				t1 = tick();
				i0 = instr() - i0;
				if (i0 < ins_d)
					ins_d = i0;
				if (t1 - t0 < best_d)
					best_d = t1 - t0;
				spent += t1 - t0;
			}
			if (do_decomp && memcmp(in, dec, in_size)) {
				fprintf(stderr, "MISMATCH %s level %d\n", base, levels[i]);
				return 1;
			}
			printf("%s %d %zu %zu %08x %llu %llu %llu %llu\n", base, levels[i],
			       in_size, total, hash,
			       (unsigned long long)(do_comp ? best_c : 0),
			       (unsigned long long)(do_decomp ? best_d : 0),
			       (unsigned long long)(do_comp ? ins_c : 0),
			       (unsigned long long)(do_decomp ? ins_d : 0));
			fflush(stdout);
			free(out);
			libdeflate_free_compressor(cmp);
		}
		libdeflate_free_decompressor(d);
		free(dec);
		free(csizes);
		free(in);
	}
	return 0;
}
