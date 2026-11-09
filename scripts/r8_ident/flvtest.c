/*
 * flvtest.c - end-to-end differential test harness for libdeflate compression.
 *
 * Generates deterministic synthetic inputs, compresses them at the given
 * levels (one reused compressor per level, so stale state carries across
 * calls) into several output buffer sizes, and prints one digest line per
 * case.  Run the same binary linked against two libraries and diff the
 * outputs.
 *
 * usage: flvtest MODE LEVELS FILE...
 *   MODE = small | mid | mid2 | big | wrap | all
 *   LEVELS = comma-separated list
 *   The first FILE should be text and the last one binary.
 *
 * Each line ends with two hashes: out= covers only the compressed bytes of
 * the bound-sized call, and all= also covers the return values and whole
 * buffers of the calls with other buffer sizes.
 *
 * Adapted for CI from handoff/tools/identity/flvtest.c: the level list, the
 * out= hash, no compiler builtins, no floating point, sizes that don't depend
 * on the order of the levels, integers hashed least significant byte first,
 * and the summary on stdout only.
 */
#include <libdeflate.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs;
static uint64_t mix64(uint64_t z)
{
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}
static uint64_t rnd64(void)
{
	return mix64(rs += 0x9E3779B97F4A7C15ULL);
}
static uint32_t rnd(uint32_t n) { return n ? (uint32_t)(rnd64() % n) : 0; }

static unsigned ctz64(uint64_t v)
{
	unsigned n = 0;

	while (!(v & 1)) {
		v >>= 1;
		n++;
	}
	return n;
}

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ULL;
	return h;
}

static uint64_t fnv_u64(uint64_t v, uint64_t h)
{
	uint8_t b[8];

	for (int i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (8 * i));
	return fnv(b, 8, h);
}

struct file { uint8_t *data; size_t size; };
static struct file files[64];
static int nfiles;

static const char phrase[] = "The quick brown fox jumps over 37 lz!";

#define NKINDS 16

static void gen(int kind, uint8_t *buf, size_t n, uint64_t seed)
{
	size_t i;

	rs = seed * 0x2545F4914F6CDD1DULL + kind;
	switch (kind) {
	case 0: memset(buf, 0, n); break;
	case 1: for (i = 0; i < n; i++) buf[i] = (uint8_t)rnd64(); break;
	case 2: for (i = 0; i < n; i++) buf[i] = phrase[i % 37]; break;
	case 3: for (i = 0; i < n; i++) buf[i] = (uint8_t)('a' + rnd(3)); break;
	case 4: { /* text from file 0 */
		struct file *f = &files[0];
		size_t off = f->size > n ? rnd((uint32_t)(f->size - n)) : 0;
		for (i = 0; i < n; i++) buf[i] = f->data[(off + i) % f->size];
		break; }
	case 5: /* geometric p=1/2 -> deep litlen trees */
		for (i = 0; i < n; i++) {
			uint64_t v = rnd64() | (1ULL << 63);
			buf[i] = (uint8_t)ctz64(v);
		}
		break;
	case 6: /* geometric ratio 0.7, up to 255 */
		for (i = 0; i < n; i++) {
			unsigned k = 0;
			while (k < 255 && rnd(10) < 7) k++;
			buf[i] = (uint8_t)k;
		}
		break;
	case 7: for (i = 0; i < n; i++) buf[i] = (uint8_t)i; break;
	case 8: { /* two symbols, random runs */
		uint8_t s = 'x';
		for (i = 0; i < n; ) {
			unsigned r = 1 + rnd(20);
			while (r-- && i < n) buf[i++] = s;
			s ^= 'x' ^ 'y';
		}
		break; }
	case 9: /* concatenation of random pieces of different files */
		for (i = 0; i < n; ) {
			struct file *f = &files[rnd(nfiles)];
			size_t len = 1 + rnd(5000), off;
			if (len > n - i) len = n - i;
			if (len > f->size) len = f->size;
			off = rnd((uint32_t)(f->size - len + 1));
			memcpy(buf + i, f->data + off, len);
			i += len;
		}
		break;
	case 10: /* match stress: all lengths and offsets */
		for (i = 0; i < n; ) {
			if (i > 0 && rnd(2)) {
				size_t maxoff = i < 32768 ? i : 32768;
				size_t off = 1 + (rnd(4) ? rnd((uint32_t)maxoff) :
						  rnd((uint32_t)(maxoff < 64 ? maxoff : 64)));
				size_t len = 3 + rnd(256);
				while (len-- && i < n) { buf[i] = buf[i - off]; i++; }
			} else {
				buf[i++] = (uint8_t)('a' + rnd(20));
			}
		}
		break;
	case 11: /* random seed then long copies */
		for (i = 0; i < n && i < 1024; i++) buf[i] = (uint8_t)rnd64();
		while (i < n) {
			size_t off = 1 + rnd((uint32_t)(i < 32768 ? i : 32768));
			size_t len = 258;
			while (len-- && i < n) { buf[i] = buf[i - off]; i++; }
		}
		break;
	case 12: { /* binary file (last file given) */
		struct file *f = &files[nfiles - 1];
		size_t off = f->size > n ? rnd((uint32_t)(f->size - n)) : 0;
		for (i = 0; i < n; i++) buf[i] = f->data[(off + i) % f->size];
		break; }
	case 13: /* ratio 0.618 (Fibonacci-like) over 0..63 */
		for (i = 0; i < n; i++) {
			unsigned k = 0;
			while (k < 63 && rnd(1000) < 618) k++;
			buf[i] = (uint8_t)(k * 4 + 1);
		}
		break;
	case 14: /* random bytes in short runs (offset-1 matches) */
		for (i = 0; i < n; ) {
			uint8_t b = (uint8_t)rnd64();
			unsigned r = 1 + rnd(10);
			while (r-- && i < n) buf[i++] = b;
		}
		break;
	case 15: /* mostly zeros, sparse random bytes, some long distance repeats */
		for (i = 0; i < n; i++) {
			buf[i] = rnd(50) ? 0 : (uint8_t)rnd64();
			if (i >= 32768 && rnd(500) == 0) {
				size_t len = 3 + rnd(300), off = 20000 + rnd(12768);
				while (len-- && i < n) { buf[i] = buf[i - off]; i++; }
			}
		}
		break;
	}
}

static int levels[13], nlevels;
static struct libdeflate_compressor *comp[13];
static struct libdeflate_decompressor *dec;
static uint8_t *obuf, *dbuf;
static size_t obuf_size;
static unsigned long long ncases, nfail;

static void one_case(int kind, size_t n, uint64_t seed, int level,
		     const uint8_t *in, int wrap)
{
	struct libdeflate_compressor *c = comp[level];
	size_t bound, ret0, sizes[8];
	int nsizes = 0, j;
	uint64_t h = 1469598103934665603ULL, ho;

	if (wrap == 0)
		bound = libdeflate_deflate_compress_bound(c, n);
	else if (wrap == 1)
		bound = libdeflate_zlib_compress_bound(c, n);
	else
		bound = libdeflate_gzip_compress_bound(c, n);
	if (bound > obuf_size) { fprintf(stderr, "bound too big\n"); exit(1); }

	memset(obuf, 0xA5, bound);
	if (wrap == 0)
		ret0 = libdeflate_deflate_compress(c, in, n, obuf, bound);
	else if (wrap == 1)
		ret0 = libdeflate_zlib_compress(c, in, n, obuf, bound);
	else
		ret0 = libdeflate_gzip_compress(c, in, n, obuf, bound);
	if (ret0 == 0) { printf("FAIL bound kind=%d n=%zu L%d\n", kind, n, level); nfail++; return; }
	h = fnv(obuf, ret0, h);
	ho = h;

	/* round trip */
	{
		size_t actual_in = 0, actual_out = 0;
		enum libdeflate_result r;
		if (wrap == 0)
			r = libdeflate_deflate_decompress_ex(dec, obuf, ret0, dbuf, n, &actual_in, &actual_out);
		else if (wrap == 1)
			r = libdeflate_zlib_decompress_ex(dec, obuf, ret0, dbuf, n, &actual_in, &actual_out);
		else
			r = libdeflate_gzip_decompress_ex(dec, obuf, ret0, dbuf, n, &actual_in, &actual_out);
		if (r != LIBDEFLATE_SUCCESS || actual_in != ret0 || actual_out != n ||
		    (n && memcmp(dbuf, in, n) != 0)) {
			printf("FAIL roundtrip kind=%d n=%zu L%d r=%d\n", kind, n, level, (int)r);
			nfail++;
		}
	}

	sizes[nsizes++] = ret0;
	sizes[nsizes++] = ret0 - 1;
	sizes[nsizes++] = ret0 / 2;
	sizes[nsizes++] = ret0 > 5 ? 5 : 1;
	sizes[nsizes++] = 1;
	sizes[nsizes++] = 0;
	/* a size that depends only on the case, not on the cases before it */
	sizes[nsizes++] = (size_t)(mix64(seed * 1000003 + (uint64_t)kind * 8191 +
					 (uint64_t)level * 131 + (uint64_t)wrap * 7 +
					 (uint64_t)n) % ((uint64_t)ret0 + 1));
	sizes[nsizes++] = ret0 + 1;
	for (j = 0; j < nsizes; j++) {
		size_t sz = sizes[j], r;
		memset(obuf, 0x5A, sz + 16);
		if (wrap == 0)
			r = libdeflate_deflate_compress(c, in, n, obuf, sz);
		else if (wrap == 1)
			r = libdeflate_zlib_compress(c, in, n, obuf, sz);
		else
			r = libdeflate_gzip_compress(c, in, n, obuf, sz);
		h = fnv_u64(r, h);
		/* hash the whole buffer, including bytes written on failure */
		h = fnv(obuf, sz + 16, h);
	}
	printf("w%d k%d n%zu s%llu L%d %zu out=%016llx all=%016llx\n", wrap, kind,
	       n, (unsigned long long)seed, level, ret0,
	       (unsigned long long)ho, (unsigned long long)h);
	ncases++;
}

static uint8_t *load(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p;
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc(*size + 1);
	if (fread(p, 1, *size, f) != *size) { perror("read"); exit(1); }
	fclose(f);
	return p;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "all";
	int all = !strcmp(mode, "all");
	size_t maxn = 8u << 20;
	uint8_t *in;
	int li, kind, i;
	size_t n;
	char *s;

	if (argc < 4) {
		fprintf(stderr, "usage: flvtest MODE LEVELS FILE...\n");
		return 2;
	}
	s = argv[2];
	while (*s && nlevels < 13) {
		levels[nlevels] = (int)strtol(s, &s, 10);
		if (levels[nlevels] < 0 || levels[nlevels] > 12) {
			fprintf(stderr, "bad level\n");
			return 2;
		}
		nlevels++;
		if (*s == ',')
			s++;
	}
	for (i = 3; i < argc && nfiles < 64; i++) {
		files[nfiles].data = load(argv[i], &files[nfiles].size);
		if (files[nfiles].size == 0) {
			fprintf(stderr, "empty file\n");
			return 2;
		}
		nfiles++;
	}

	in = malloc(maxn);
	obuf_size = maxn + maxn / 8 + 4096;
	obuf = malloc(obuf_size + 64);
	dbuf = malloc(maxn + 64);
	dec = libdeflate_alloc_decompressor();
	for (li = 0; li < nlevels; li++) {
		comp[levels[li]] = libdeflate_alloc_compressor(levels[li]);
		if (!comp[levels[li]] || !dec || !in || !obuf || !dbuf) {
			fprintf(stderr, "alloc failed\n");
			return 2;
		}
	}

	/* tiny inputs: every size 0..300 for all kinds, 2 seeds */
	if (all || !strcmp(mode, "small")) {
		for (kind = 0; kind < NKINDS; kind++)
			for (n = 0; n <= 300; n++)
				for (uint64_t seed = 1; seed <= 2; seed++) {
					gen(kind, in, n, seed);
					for (li = 0; li < nlevels; li++)
						one_case(kind, n, seed, levels[li], in, 0);
				}
	}
	/*
	 * mid sizes 301 .. ~200 KB, geometric steps (x1.09) with jitter;
	 * "mid" is the part below 50 KB and "mid2" is the rest
	 */
	if (all || !strcmp(mode, "mid") || !strcmp(mode, "mid2")) {
		uint64_t seed = 100;
		for (uint64_t x = 301; x < 200000; x = x * 109 / 100) {
			int skip = !all && (x < 50000) != !strcmp(mode, "mid");

			for (kind = 0; kind < NKINDS; kind++) {
				seed++;
				if (skip)
					continue;
				rs = seed;
				n = (size_t)x + rnd(97);
				gen(kind, in, n, seed);
				for (li = 0; li < nlevels; li++)
					one_case(kind, n, seed, levels[li], in, 0);
			}
		}
	}
	/* big inputs */
	if (all || !strcmp(mode, "big")) {
		static const struct { int kind; size_t n; } big[] = {
			{ 1, 8u << 20 }, { 0, 8u << 20 }, { 2, 8u << 20 },
			{ 3, 1u << 20 }, { 9, 3u << 20 }, { 10, 1u << 20 },
			{ 5, 1u << 20 }, { 13, 1u << 20 }, { 15, 2u << 20 },
			{ 11, 1u << 20 }, { 14, 1u << 20 }, { 6, 1u << 20 },
		};
		for (i = 0; i < (int)(sizeof(big) / sizeof(big[0])); i++) {
			gen(big[i].kind, in, big[i].n, 7777 + i);
			for (li = 0; li < nlevels; li++)
				one_case(big[i].kind, big[i].n, 7777 + i,
					 levels[li], in, 0);
		}
	}
	/* zlib and gzip wrappers */
	if (all || !strcmp(mode, "wrap")) {
		for (kind = 0; kind < NKINDS; kind++)
			for (n = 0; n < 70000; n = n * 3 + 1) {
				gen(kind, in, n, 55);
				for (li = 0; li < nlevels; li++) {
					one_case(kind, n, 55, levels[li], in, 1);
					one_case(kind, n, 55, levels[li], in, 2);
				}
			}
	}
	printf("cases=%llu failures=%llu\n", ncases, nfail);
	return nfail != 0;
}
