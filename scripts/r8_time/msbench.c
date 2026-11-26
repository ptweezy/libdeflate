/*
 * msbench LEVEL CHUNK REPS FILE...  (test-only, Windows)
 * Prints "name ns_per_byte out_size out_hash" per file: the minimum time of
 * reps 1..REPS-1 (rep 0 also hashes the output).  MSB_CPU pins the thread.
 *
 * LEVEL dN times decompression instead: every chunk is compressed once at
 * level N, untimed, and each rep decompresses all of them (rep 0 also checks
 * them against the input).  ns_per_byte is then per uncompressed byte, and
 * out_size and out_hash are those of the compressed data.
 */
#include <libdeflate.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int main(int argc, char **argv)
{
	int decomp = argv[1][0] == 'd';
	int level = atoi(argv[1] + decomp);
	size_t chunk = (size_t)atoi(argv[2]);
	int reps = atoi(argv[3]);
	int a;
	LARGE_INTEGER fq, t0, t1;
	const char *cpu = getenv("MSB_CPU");
	struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
	struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
	QueryPerformanceFrequency(&fq);
	if (cpu)
		SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << atoi(cpu));
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	for (a = 4; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		size_t n, cap, i, pos, out = 0;
		unsigned char *in, *ob, *cb = NULL;
		size_t *csz = NULL;
		uint64_t h = 1469598103934665603ULL;
		double best = 1e30;
		int r;
		if (!f) { fprintf(stderr, "open %s\n", argv[a]); return 1; }
		fseek(f, 0, SEEK_END); n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
		in = malloc(n); if (fread(in, 1, n, f) != n) return 1; fclose(f);
		cap = libdeflate_deflate_compress_bound(c, chunk ? chunk : n);
		ob = malloc(cap);
		if (decomp) {
			/* the compressed chunks back to back, and their sizes */
			size_t nch = chunk ? (n + chunk - 1) / chunk : 1, k = 0;
			cb = malloc(nch * cap);
			csz = malloc(nch * sizeof(*csz));
			for (pos = 0; pos < n; k++) {
				size_t len = chunk && n - pos > chunk ? chunk : n - pos;
				csz[k] = libdeflate_deflate_compress(c, in + pos, len, cb + out, cap);
				if (!csz[k]) { fprintf(stderr, "compress %s\n", argv[a]); return 1; }
				for (i = 0; i < csz[k]; i++) h = (h ^ cb[out + i]) * 1099511628211ULL;
				out += csz[k]; pos += len;
			}
		}
		for (r = 0; r < reps; r++) {
			double dt;
			size_t cp = 0, k = 0;
			if (!decomp)
				out = 0;
			QueryPerformanceCounter(&t0);
			for (pos = 0; pos < n; k++) {
				size_t len = chunk && n - pos > chunk ? chunk : n - pos;
				if (decomp) {
					if (libdeflate_deflate_decompress(d, cb + cp, csz[k], ob, len, NULL) != LIBDEFLATE_SUCCESS ||
					    (r == 0 && memcmp(ob, in + pos, len) != 0)) {
						fprintf(stderr, "decompress %s\n", argv[a]);
						return 1;
					}
					cp += csz[k];
				} else {
					size_t o = libdeflate_deflate_compress(c, in + pos, len, ob, cap);
					if (r == 0)
						for (i = 0; i < o; i++) h = (h ^ ob[i]) * 1099511628211ULL;
					out += o;
				}
				pos += len;
			}
			QueryPerformanceCounter(&t1);
			dt = (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
			if (r > 0 && dt < best) best = dt;
		}
		printf("%s %.3f %llu %016llx\n", argv[a], best * 1e9 / (double)n,
		       (unsigned long long)out, (unsigned long long)h);
		free(in); free(ob); free(cb); free(csz);
	}
	return 0;
}
