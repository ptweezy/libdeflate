/*
 * mdrv.c - portable (stdio-only) compression digest driver, for comparing
 * builds across compilers/platforms (gcc/clang on Linux, MSVC on Windows).
 *
 * usage: mdrv LEVELS CHUNK FILE...
 *	LEVELS is a comma-separated list.
 * For each file and level: one compressor, chunks of CHUNK bytes (0 = whole
 * file).  For every chunk: compress into a bound-size buffer (hashed into H1,
 * the same FNV-1a as ldbench -H), then again into buffers of exactly the
 * compressed size, size-1, size/2 and 1 byte, hashing the return values and
 * the full output buffer contents (H2), and the zlib and gzip wrappers (H3).
 * Prints: name level chunk total H1 H2 H3
 *
 * Adapted for CI from handoff/tools/identity/mdrv.c: a level list instead of
 * a range, and the chunk size in the output.
 */
#include <libdeflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static u64 fnv(const unsigned char *p, size_t n, u64 h)
{
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ULL;
	return h;
}

static u64 fnv_sz(size_t v, u64 h)
{
	unsigned char b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (unsigned char)((u64)v >> (8 * i));
	return fnv(b, 8, h);
}

int main(int argc, char **argv)
{
	int levels[13], nlevels = 0, li, fi;
	size_t chunk;
	char *s;

	if (argc < 4) {
		fprintf(stderr, "usage: mdrv LEVELS CHUNK FILE...\n");
		return 2;
	}
	s = argv[1];
	while (*s && nlevels < 13) {
		levels[nlevels++] = (int)strtol(s, &s, 10);
		if (*s == ',')
			s++;
	}
	chunk = (size_t)strtoul(argv[2], NULL, 0);
	for (fi = 3; fi < argc; fi++) {
		FILE *f = fopen(argv[fi], "rb");
		unsigned char *in, *out, *small;
		size_t n, cs, bound, i;
		const char *name, *bs;

		if (!f) { perror(argv[fi]); return 1; }
		fseek(f, 0, SEEK_END);
		n = (size_t)ftell(f);
		fseek(f, 0, SEEK_SET);
		in = malloc(n + 1);
		if (fread(in, 1, n, f) != n) { perror("read"); return 1; }
		fclose(f);
		name = strrchr(argv[fi], '/');
		bs = strrchr(argv[fi], '\\');
		if (bs && (!name || bs > name))
			name = bs;
		name = name ? name + 1 : argv[fi];
		cs = chunk ? chunk : n;
		for (li = 0; li < nlevels; li++) {
			int level = levels[li];
			struct libdeflate_compressor *c =
				libdeflate_alloc_compressor(level);
			u64 h1 = 1469598103934665603ULL, h2 = h1, h3 = h1;
			size_t total = 0;

			if (!c) { fprintf(stderr, "alloc failed\n"); return 1; }
			bound = libdeflate_zlib_compress_bound(c, cs) + 32;
			out = malloc(bound);
			small = malloc(bound);
			for (i = 0; i < n; i += cs) {
				size_t len = n - i < cs ? n - i : cs;
				size_t clen, r;
				size_t tries[4];
				int t;

				clen = libdeflate_deflate_compress(c, in + i, len,
					out, libdeflate_deflate_compress_bound(c, len));
				if (clen == 0) { fprintf(stderr, "compress failed\n"); return 1; }
				total += clen;
				h1 = fnv(out, clen, h1);
				tries[0] = clen; tries[1] = clen - 1;
				tries[2] = clen / 2; tries[3] = 1;
				for (t = 0; t < 4; t++) {
					memset(small, 0xA5, bound);
					r = libdeflate_deflate_compress(c, in + i, len,
									small, tries[t]);
					h2 = fnv_sz(r, h2);
					h2 = fnv(small, tries[t] + 8 < bound ? tries[t] + 8 : bound, h2);
				}
				r = libdeflate_zlib_compress(c, in + i, len, small, bound);
				h3 = fnv(small, r, fnv_sz(r, h3));
				r = libdeflate_gzip_compress(c, in + i, len, small, bound);
				h3 = fnv(small, r, fnv_sz(r, h3));
			}
			printf("%s L%d c%lu %lu out=%016llx short=%016llx wrap=%016llx\n",
			       name, level, (unsigned long)chunk,
			       (unsigned long)total, h1, h2, h3);
			fflush(stdout);
			free(out);
			free(small);
			libdeflate_free_compressor(c);
		}
		free(in);
	}
	return 0;
}
