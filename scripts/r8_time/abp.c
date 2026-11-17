/*
 * abp.c - interleaved A/B/... timing of libdeflate builds loaded with dlopen()
 * (test-only; the portable variant of the local ab.c: monotonic clock only).
 *
 * usage: ab [-l LEVEL] [-s CHUNK] [-r REPS] [-t SECONDS] [-d] LIB.so... -- FILE...
 *
 * Each FILE is compressed in independent chunks of CHUNK bytes (0 = whole
 * file) by every LIB, round-robin, REPS times (or until SECONDS have passed).
 * For each LIB and FILE, the minimum time over the reps is kept, in
 * nanoseconds of CLOCK_MONOTONIC_RAW (CLOCK_MONOTONIC if unavailable).  The compressed
 * outputs of all LIBs must be identical.  -d times decompression instead (of
 * LIB[0]'s output).
 *
 * Output per file: name, ratio, for each LIB the min time per byte, then for
 * each LIB after the first its speedup over LIB[0] in percent (positive =
 * faster).
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

typedef void *(*alloc_c_t)(int);
typedef size_t (*compress_t)(void *, const void *, size_t, void *, size_t);
typedef size_t (*bound_t)(void *, size_t);
typedef void (*free_t)(void *);
typedef void *(*alloc_d_t)(void);
typedef int (*decompress_t)(void *, const void *, size_t, void *, size_t, size_t *);

struct lib {
	const char *path;
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
	buf = malloc(size ? size : 1);
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
			fprintf(stderr, "decompression failed\n");
			exit(1);
		}
		cpos += csizes[i++];
		pos += len;
	} while (pos < n);
}

int main(int argc, char **argv)
{
	int level = 6, reps = 15, opt, nlibs = 0, decomp = 0;
	size_t chunk = 0;
	double max_secs = 0;
	struct lib libs[16];
	int i;

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
	for (; optind < argc && strcmp(argv[optind], "--") != 0; optind++) {
		void *h = dlopen(argv[optind], RTLD_NOW | RTLD_LOCAL);
		struct lib *L = &libs[nlibs++];

		if (!h) {
			fprintf(stderr, "%s\n", dlerror());
			return 1;
		}
		L->path = argv[optind];
		L->alloc_c = xdlsym(h, "libdeflate_alloc_compressor");
		L->compress = xdlsym(h, "libdeflate_deflate_compress");
		L->bound = xdlsym(h, "libdeflate_deflate_compress_bound");
		L->free_c = xdlsym(h, "libdeflate_free_compressor");
		L->alloc_d = xdlsym(h, "libdeflate_alloc_decompressor");
		L->decompress = xdlsym(h, "libdeflate_deflate_decompress");
		L->comp = L->alloc_c(level);
		L->decomp = L->alloc_d();
	}
	if (optind >= argc || nlibs == 0)
		return 2;
	optind++;

	fprintf(stderr, "timer: monotonic ns\n");

	for (; optind < argc; optind++) {
		size_t n, nchunks, total_ref = 0;
		uint8_t *in = read_file(argv[optind], &n);
		size_t outcap = n + n / 8 + 1024 * (n / 1024 + 2);
		uint8_t *out = malloc(outcap);
		uint8_t *ref = malloc(outcap);
		uint8_t *dout = malloc(n + 1);
		size_t *csizes;
		uint64_t best[16];
		const char *name = strrchr(argv[optind], '/');
		double t0;

		name = name ? name + 1 : argv[optind];
		nchunks = chunk ? (n + chunk - 1) / chunk : 1;
		csizes = malloc((nchunks + 1) * sizeof(size_t));

		/* Check that all libs give identical output; warm up. */
		for (i = 0; i < nlibs; i++) {
			size_t t = compress_chunks(&libs[i], in, n, chunk,
						   i ? out : ref,
						   i ? NULL : csizes);
			if (i == 0) {
				total_ref = t;
			} else if (t != total_ref || memcmp(out, ref, t) != 0) {
				fprintf(stderr, "%s: OUTPUT DIFFERS for %s\n",
					name, libs[i].path);
				return 1;
			}
			best[i] = UINT64_MAX;
		}
		if (decomp) {
			decompress_chunks(&libs[0], ref, csizes, n, chunk, dout);
			if (memcmp(dout, in, n) != 0) {
				fprintf(stderr, "roundtrip failed\n");
				return 1;
			}
		}

		t0 = now();
		for (int r = 0; r < reps; r++) {
			for (int k = 0; k < nlibs; k++) {
				/* rotate the order each rep */
				int j = (k + r) % nlibs;
				uint64_t c0, c1;

				if (decomp) {
					c0 = timer();
					decompress_chunks(&libs[j], ref, csizes,
							  n, chunk, dout);
					c1 = timer();
				} else {
					c0 = timer();
					compress_chunks(&libs[j], in, n, chunk,
							out, NULL);
					c1 = timer();
				}
				if (c1 - c0 < best[j])
					best[j] = c1 - c0;
			}
			if (max_secs && now() - t0 > max_secs && r >= 2)
				break;
		}
		printf("%-10s %6.2f%%", name, 100.0 * total_ref / n);
		for (i = 0; i < nlibs; i++)
			printf(" %8.3f", (double)best[i] / n);
		for (i = 1; i < nlibs; i++)
			printf(" %+7.2f%%",
			       100.0 * ((double)best[0] / best[i] - 1));
		printf("\n");
		fflush(stdout);
		free(in);
		free(out);
		free(ref);
		free(dout);
		free(csizes);
	}
	return 0;
}
