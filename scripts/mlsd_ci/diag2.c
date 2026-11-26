/*
 * diag2.c - test-only: per-block decode times and Huffman code statistics.
 * usage: diag2 TAG LEVEL REPS FILE...   (prints CSV lines)
 * Built against a decompressor compiled with -DMLSD_DIAG (forced lanes, or
 * lanes off); the compressor is stock libdeflate.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libdeflate.h"

struct mlsd_diag_rec {
	uint32_t type, len_space, lit_avg100, lit_short1000, bpb100, out_bytes;
	uint64_t ticks;
};
extern struct mlsd_diag_rec mlsd_diag_log[1 << 18];
extern unsigned long mlsd_diag_n;
struct libdeflate_decompressor *B_alloc_decompressor(void);
enum libdeflate_result B_deflate_decompress(struct libdeflate_decompressor *,
	const void *, size_t, void *, size_t, size_t *);

int main(int argc, char **argv)
{
	const char *tag = argv[1];
	int level = atoi(argv[2]), reps = atoi(argv[3]), f;
	struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
	struct libdeflate_decompressor *d = B_alloc_decompressor();

	for (f = 4; f < argc; f++) {
		FILE *fp = fopen(argv[f], "rb");
		const char *name = strrchr(argv[f], '/');
		size_t n, cb, cl;
		unsigned char *in, *cbuf, *out;
		unsigned long nb = 0, i;
		uint64_t *best;
		int r;

		name = name ? name + 1 : argv[f];
		if (!fp) { perror(argv[f]); return 1; }
		fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
		in = malloc(n); if (fread(in, 1, n, fp) != n) return 1; fclose(fp);
		cb = libdeflate_deflate_compress_bound(c, n);
		cbuf = malloc(cb); out = malloc(n);
		cl = libdeflate_deflate_compress(c, in, n, cbuf, cb);
		best = NULL;
		for (r = 0; r < reps; r++) {
			mlsd_diag_n = 0;
			if (B_deflate_decompress(d, cbuf, cl, out, n, NULL) != 0 ||
			    memcmp(in, out, n) != 0) {
				fprintf(stderr, "%s: decompression failed\n", name);
				return 1;
			}
			if (r == 0) {
				nb = mlsd_diag_n;
				best = malloc(nb * sizeof(*best));
				for (i = 0; i < nb; i++)
					best[i] = mlsd_diag_log[i].ticks;
			} else {
				if (mlsd_diag_n != nb) { fprintf(stderr, "block count changed\n"); return 1; }
				for (i = 0; i < nb; i++)
					if (mlsd_diag_log[i].ticks < best[i])
						best[i] = mlsd_diag_log[i].ticks;
			}
		}
		for (i = 0; i < nb; i++) {
			const struct mlsd_diag_rec *x = &mlsd_diag_log[i];
			printf("%s,%s,%d,%lu,%u,%u,%u,%u,%u,%u,%llu\n", tag, name,
			       level, i, x->type, x->len_space, x->lit_avg100,
			       x->lit_short1000, x->bpb100, x->out_bytes,
			       (unsigned long long)best[i]);
		}
		free(best); free(in); free(cbuf); free(out);
	}
	return 0;
}
