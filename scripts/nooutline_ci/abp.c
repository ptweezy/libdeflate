/*
 * abp.c - interleaved A/B/... timing of libdeflate builds loaded with dlopen()
 * (test-only).  This is scripts/r8_time/abp.c with three additions: the median
 * of the paired per-rep speedups, a check that every library really is a
 * separately loaded image, and a decompression check of every library.
 *
 * usage: abp [-l LEVEL] [-s CHUNK] [-r REPS] [-t SECONDS] [-d] LIB... -- FILE...
 *
 * Each FILE is compressed in independent chunks of CHUNK bytes (0 = whole
 * file) by every LIB, round-robin with the order rotated each rep, REPS times
 * (or until SECONDS have passed, but at least 3 reps).  The compressed outputs
 * of all LIBs must be identical.  -d times decompression instead (of LIB[0]'s
 * output, which every LIB must decompress correctly).
 *
 * Output per file:
 *   name ratio% ns/B[0] ... ns/B[n-1] | min%[1] ... min%[n-1] | med%[1] ... med%[n-1] | reps
 * ns/B is the minimum time over the reps per input byte (CLOCK_MONOTONIC_RAW
 * nanoseconds); min% is LIB[i]'s speedup over LIB[0] from those minimums;
 * med% is the median over reps of the same-rep speedup.  Positive = faster.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_MONOTONIC_RAW
#  define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define MAX_LIBS 16
#define MAX_REPS 1000

typedef void *(*alloc_c_t)(int);
typedef size_t (*compress_t)(void *, const void *, size_t, void *, size_t);
typedef size_t (*bound_t)(void *, size_t);
typedef void (*free_t)(void *);
typedef void *(*alloc_d_t)(void);
typedef int (*decompress_t)(void *, const void *, size_t, void *, size_t, size_t *);

struct lib {
	const char *path;
	void *handle;
	alloc_c_t alloc_c;
	compress_t compress;
	bound_t bound;
	free_t free_c;
	alloc_d_t alloc_d;
	decompress_t decompress;
	void *comp, *decomp;
};

static uint64_t timer(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *xdlsym(void *h, const char *name)
{
	void *p = dlsym(h, name);

	if (!p) {
		fprintf(stderr, "missing %s\n", name);
		exit(1);
	}
	return p;
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

static uint8_t *read_file(const char *path, size_t *size_ret)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long size;

	if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET)) {
		perror(path);
		exit(1);
	}
	buf = xmalloc(size);
	if (fread(buf, 1, size, f) != (size_t)size) {
		perror(path);
		exit(1);
	}
	fclose(f);
	*size_ret = size;
	return buf;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Compress 'in' in chunks; returns total compressed size, fills 'out'. */
static size_t compress_chunks(struct lib *L, const uint8_t *in, size_t n,
			      size_t chunk, uint8_t *out, size_t *csizes)
{
	size_t pos = 0, total = 0, i = 0;

	do {
		size_t len = (chunk == 0 || n - pos < chunk) ? n - pos : chunk;
		size_t bound = L->bound(L->comp, len);
		size_t c = L->compress(L->comp, in + pos, len, out + total, bound);

		if (c == 0) {
			fprintf(stderr, "compression failed\n");
			exit(1);
		}
		if (csizes)
			csizes[i] = c;
		i++;
		total += c;
		pos += len;
	} while (pos < n);
	return total;
}

static void decompress_chunks(struct lib *L, const uint8_t *cdata,
			      const size_t *csizes, size_t n, size_t chunk,
			      uint8_t *out)
{
	size_t pos = 0, cpos = 0, i = 0;

	do {
		size_t len = (chunk == 0 || n - pos < chunk) ? n - pos : chunk;

		if (L->decompress(L->decomp, cdata + cpos, csizes[i], out + pos,
				  len, NULL) != 0) {
			fprintf(stderr, "decompression failed (%s)\n", L->path);
			exit(1);
		}
		cpos += csizes[i++];
		pos += len;
	} while (pos < n);
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static double median(double *v, int n)
{
	qsort(v, n, sizeof(v[0]), cmp_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

int main(int argc, char **argv)
{
	int level = 6, reps = 15, opt, nlibs = 0, decomp = 0;
	size_t chunk = 0;
	double max_secs = 0;
	static struct lib libs[MAX_LIBS];
	static uint64_t times[MAX_LIBS][MAX_REPS];
	static double ratios[MAX_REPS];
	int i, j;

	while ((opt = getopt(argc, argv, "+l:s:r:t:d")) != -1) {
		switch (opt) {
		case 'l': level = atoi(optarg); break;
		case 's': chunk = strtoul(optarg, NULL, 0); break;
		case 'r': reps = atoi(optarg); break;
		case 't': max_secs = atof(optarg); break;
		case 'd': decomp = 1; break;
		default: return 2;
		}
	}
	if (reps < 1 || reps > MAX_REPS)
		return 2;
	for (; optind < argc && strcmp(argv[optind], "--") != 0; optind++) {
		void *h;
		struct lib *L;

		if (nlibs == MAX_LIBS)
			return 2;
		h = dlopen(argv[optind], RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			fprintf(stderr, "%s\n", dlerror());
			return 1;
		}
		L = &libs[nlibs];
		L->path = argv[optind];
		L->handle = h;
		L->alloc_c = xdlsym(h, "libdeflate_alloc_compressor");
		L->compress = xdlsym(h, "libdeflate_deflate_compress");
		L->bound = xdlsym(h, "libdeflate_deflate_compress_bound");
		L->free_c = xdlsym(h, "libdeflate_free_compressor");
		L->alloc_d = xdlsym(h, "libdeflate_alloc_decompressor");
		L->decompress = xdlsym(h, "libdeflate_deflate_decompress");
		/*
		 * A library that the loader handed back again (same handle or
		 * same code) would make an A/B comparison meaningless.
		 */
		for (j = 0; j < nlibs; j++) {
			if (libs[j].handle == h ||
			    (void *)libs[j].compress == (void *)L->compress) {
				fprintf(stderr, "%s and %s are the same loaded image\n",
					libs[j].path, L->path);
				return 1;
			}
		}
		L->comp = L->alloc_c(level);
		L->decomp = L->alloc_d();
		if (!L->comp || !L->decomp) {
			fprintf(stderr, "allocation failed\n");
			return 1;
		}
		nlibs++;
	}
	if (optind >= argc || nlibs == 0)
		return 2;
	optind++;

	for (; optind < argc; optind++) {
		size_t n, nchunks, total_ref = 0;
		uint8_t *in = read_file(argv[optind], &n);
		size_t outcap = n + n / 8 + 1024 * (n / 1024 + 2);
		uint8_t *out = xmalloc(outcap);
		uint8_t *ref = xmalloc(outcap);
		uint8_t *dout = xmalloc(n + 1);
		size_t *csizes;
		uint64_t best[MAX_LIBS];
		const char *name = strrchr(argv[optind], '/');
		double t0;
		int r, done = 0;

		name = name ? name + 1 : argv[optind];
		nchunks = chunk ? (n + chunk - 1) / chunk : 1;
		csizes = xmalloc((nchunks + 1) * sizeof(size_t));

		/* Check that all libs give identical output; warm up. */
		for (i = 0; i < nlibs; i++) {
			size_t t = compress_chunks(&libs[i], in, n, chunk,
						   i ? out : ref,
						   i ? NULL : csizes);
			if (i == 0) {
				total_ref = t;
			} else if (t != total_ref || memcmp(out, ref, t) != 0) {
				fprintf(stderr, "%s: OUTPUT DIFFERS for %s (level %d)\n",
					name, libs[i].path, level);
				return 1;
			}
			best[i] = UINT64_MAX;
		}
		if (decomp) {
			for (i = 0; i < nlibs; i++) {
				memset(dout, 0, n);
				decompress_chunks(&libs[i], ref, csizes, n,
						  chunk, dout);
				if (memcmp(dout, in, n) != 0) {
					fprintf(stderr, "%s: roundtrip failed for %s\n",
						name, libs[i].path);
					return 1;
				}
			}
		}

		t0 = now();
		for (r = 0; r < reps; r++) {
			for (int k = 0; k < nlibs; k++) {
				/* rotate the order each rep */
				int m = (k + r) % nlibs;
				uint64_t c0, c1;

				if (decomp) {
					c0 = timer();
					decompress_chunks(&libs[m], ref, csizes,
							  n, chunk, dout);
					c1 = timer();
				} else {
					c0 = timer();
					compress_chunks(&libs[m], in, n, chunk,
							out, NULL);
					c1 = timer();
				}
				times[m][r] = c1 - c0;
				if (c1 - c0 < best[m])
					best[m] = c1 - c0;
			}
			done = r + 1;
			if (max_secs && now() - t0 > max_secs && r >= 2)
				break;
		}
		printf("%-10s %6.2f%%", name, 100.0 * total_ref / n);
		for (i = 0; i < nlibs; i++)
			printf(" %8.4f", (double)best[i] / n);
		printf(" |");
		for (i = 1; i < nlibs; i++)
			printf(" %+7.2f%%", 100.0 * ((double)best[0] / best[i] - 1));
		printf(" |");
		for (i = 1; i < nlibs; i++) {
			for (r = 0; r < done; r++)
				ratios[r] = (double)times[0][r] / times[i][r];
			printf(" %+7.2f%%", 100.0 * (median(ratios, done) - 1));
		}
		printf(" | %d\n", done);
		fflush(stdout);
		free(in);
		free(out);
		free(ref);
		free(dout);
		free(csizes);
	}
	return 0;
}
