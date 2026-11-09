/*
 * xcheck: a deterministic digest of libdeflate's observable behavior, used to
 * check that two builds (e.g. master and a patched tree) behave identically on
 * one architecture.  Run both builds on the same inputs and diff the output.
 *
 * usage: xcheck [-l LEVELS] FILE...
 *	LEVELS is a comma-separated list (default 0-12).
 *
 * For each level, one compressor is allocated and reused for every call, as an
 * application would.  For each FILE (at most its first BIG_LIMIT bytes) and
 * each chunk schedule, every chunk is copied into its own exactly-sized heap
 * buffer and then:
 *  - compressed into a bound-sized buffer (return value + output bytes);
 *  - for the first few chunks of each schedule, also compressed into buffers
 *    of (needed - 1) and (needed / 2) bytes prefilled with 0xA5 (return value
 *    + the whole buffer), and with the zlib and gzip wrappers;
 *  - decompressed again (round trip checked);
 *  - for the first few chunks, decompressed into exact-1 and exact+7 sized
 *    buffers and after deterministic corruptions (bit flips, truncation),
 *    recording the result code, actual_in, actual_out and the whole buffer.
 * Two FNV-1a-64 hashes are printed per (level, file, schedule):
 *  out=  only the input lengths, the compressed sizes and the compressed bytes
 *        of the bound-sized calls (raw, zlib and gzip).  It doesn't depend on
 *        how the library writes into buffers, so it can be compared between
 *        platforms.
 *  all=  everything listed above.
 * Integers are hashed least significant byte first on every platform.
 *
 * Adapted for CI from handoff/tools/identity/xcheck.c: the out= hash,
 * endian-independent integer hashing, '\' as a path separator, and a fixed
 * order of the two random numbers that choose the bit to corrupt.
 */
#include <libdeflate.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_LIMIT	(256 * 1024)	/* bytes of each file used for big chunks */
#define SMALL_LIMIT	(48 * 1024)	/* bytes used when the chunks are small */
#define EXTRA_CHUNKS	4		/* chunks per schedule with extra checks */

static uint64_t h;	/* everything */
static uint64_t ho;	/* compressed output only */

static void hash_bytes(const void *p, size_t n)
{
	const unsigned char *b = p;

	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 1099511628211ULL;
}

static void hash_u64(uint64_t v)
{
	unsigned char b[8];

	for (int i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	hash_bytes(b, 8);
}

/* Compressed output goes into both hashes. */
static void hash_out_bytes(const void *p, size_t n)
{
	const unsigned char *b = p;

	hash_bytes(p, n);
	for (size_t i = 0; i < n; i++)
		ho = (ho ^ b[i]) * 1099511628211ULL;
}

static void hash_out_u64(uint64_t v)
{
	unsigned char b[8];

	for (int i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	hash_out_bytes(b, 8);
}

static uint32_t lcg_state;

static uint32_t lcg(void)
{
	lcg_state = lcg_state * 1103515245u + 12345u;
	return lcg_state >> 1;
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	return p;
}

static struct libdeflate_decompressor *d;

/* Decompress @in into a buffer of @avail bytes and hash everything observable. */
static void hash_decompress(const unsigned char *in, size_t in_n, size_t avail)
{
	unsigned char *out = xmalloc(avail);
	size_t actual_in = 12345, actual_out = 54321;
	enum libdeflate_result r;

	memset(out, 0x5A, avail);
	r = libdeflate_deflate_decompress_ex(d, in, in_n, out, avail,
					     &actual_in, &actual_out);
	hash_u64((uint64_t)r);
	hash_u64(actual_in);
	hash_u64(actual_out);
	hash_bytes(out, avail);
	/* The variant without actual_out must agree too. */
	memset(out, 0x5A, avail);
	r = libdeflate_deflate_decompress_ex(d, in, in_n, out, avail,
					     &actual_in, NULL);
	hash_u64((uint64_t)r);
	hash_u64(actual_in);
	hash_bytes(out, avail);
	free(out);
}

static int extra_checks(struct libdeflate_compressor *c,
			const unsigned char *in, size_t len,
			const unsigned char *comp, size_t cs)
{
	size_t sizes[2];
	size_t bound;
	unsigned char *buf, *mut;

	sizes[0] = cs ? cs - 1 : 0;
	sizes[1] = cs / 2;
	/* Too-small output buffers. */
	for (int k = 0; k < 2; k++) {
		size_t n = sizes[k];

		buf = xmalloc(n + 16);
		memset(buf, 0xA5, n + 16);
		hash_u64(libdeflate_deflate_compress(c, in, len, buf, n));
		hash_bytes(buf, n + 16);
		free(buf);
	}
	/* zlib and gzip wrappers. */
	bound = libdeflate_zlib_compress_bound(c, len);
	buf = xmalloc(bound);
	memset(buf, 0, bound);
	{
		/* Hash only what was written: the rest is uninitialized. */
		size_t zs = libdeflate_zlib_compress(c, in, len, buf, bound);

		hash_out_u64(zs);
		hash_out_bytes(buf, zs);
	}
	free(buf);
	bound = libdeflate_gzip_compress_bound(c, len);
	buf = xmalloc(bound);
	memset(buf, 0, bound);
	{
		size_t gs = libdeflate_gzip_compress(c, in, len, buf, bound);

		hash_out_u64(gs);
		hash_out_bytes(buf, gs);
	}
	free(buf);

	/* Decompression with too-small and larger output buffers. */
	hash_decompress(comp, cs, len ? len - 1 : 0);
	hash_decompress(comp, cs, len + 7);
	if (len > 64)
		hash_decompress(comp, cs, 64);

	/* Corrupted and truncated streams. */
	mut = xmalloc(cs);
	for (int k = 0; k < 6 && cs; k++) {
		/* two statements: the order of the calls must not be up to the compiler */
		size_t pos = lcg() % cs;
		unsigned bit = lcg() % 8;

		memcpy(mut, comp, cs);
		mut[pos] ^= (unsigned char)(1u << bit);
		if (k >= 3)	/* also flip a second bit near the end */
			mut[cs - 1 - lcg() % (cs < 8 ? cs : 8)] ^= 0x10;
		hash_decompress(mut, cs, len);
		hash_decompress(mut, cs, len + 300);
	}
	if (cs > 1) {
		hash_decompress(comp, cs / 2, len);
		hash_decompress(comp, cs - 1, len);
	}
	free(mut);
	return 0;
}

static int run_schedule(struct libdeflate_compressor *c, const unsigned char *data,
			size_t n, const char *name, int level, const char *sched,
			size_t fixed, int mixed)
{
	size_t limit, off = 0, nchunks = 0, total = 0;

	h = 1469598103934665603ULL;
	ho = 1469598103934665603ULL;
	lcg_state = 0x9E3779B9u ^ (uint32_t)(level * 7919) ^ (uint32_t)n;
	if (mixed)
		limit = n < BIG_LIMIT ? n : BIG_LIMIT;
	else if (fixed == 0)
		limit = n < BIG_LIMIT ? n : BIG_LIMIT;
	else if (fixed < 4096)
		limit = n < SMALL_LIMIT ? n : SMALL_LIMIT;
	else
		limit = n < BIG_LIMIT ? n : BIG_LIMIT;

	do {
		size_t len;
		unsigned char *in, *out, *back;
		size_t bound, cs, actual;

		if (mixed) {
			uint32_t r = lcg();

			switch (r % 4) {
			case 0:
				len = r % 70000;
				break;
			case 1:
				len = r % 5000;
				break;
			case 2:
				len = r % 600;
				break;
			default:
				len = r % 40;
				break;
			}
		} else {
			len = fixed ? fixed : limit;
		}
		if (len > limit - off)
			len = limit - off;
		in = xmalloc(len);
		memcpy(in, data + off, len);
		bound = libdeflate_deflate_compress_bound(c, len);
		out = xmalloc(bound);
		cs = libdeflate_deflate_compress(c, in, len, out, bound);
		if (cs == 0) {
			fprintf(stderr, "%s L%d %s: compress failed\n", name,
				level, sched);
			return 1;
		}
		hash_out_u64(len);
		hash_out_u64(cs);
		hash_out_bytes(out, cs);
		back = xmalloc(len);
		if (libdeflate_deflate_decompress(d, out, cs, back, len,
						  &actual) != LIBDEFLATE_SUCCESS ||
		    actual != len || memcmp(back, in, len) != 0) {
			fprintf(stderr, "%s L%d %s: round trip failed at %zu\n",
				name, level, sched, off);
			return 1;
		}
		if (nchunks < EXTRA_CHUNKS || (mixed && nchunks % 8 == 0))
			extra_checks(c, in, len, out, cs);
		free(in);
		free(out);
		free(back);
		total += cs;
		nchunks++;
		off += len;
	} while (off < limit);

	printf("L%d %s %s chunks=%zu compressed=%zu out=%016llx all=%016llx\n",
	       level, name, sched, nchunks, total, (unsigned long long)ho,
	       (unsigned long long)h);
	return 0;
}

static const size_t fixed_sizes[] = {
	0 /* whole (up to BIG_LIMIT) */, 65536, 32769, 8192, 4097, 4096, 3000,
	2049, 2048, 2047, 1536, 1024, 777, 512, 333, 200, 100, 64, 17, 5, 1,
};

int main(int argc, char **argv)
{
	int levels[13], nlevels = 0;
	int a = 1;

	if (argc > 2 && strcmp(argv[1], "-l") == 0) {
		char *s = argv[2];

		while (*s && nlevels < 13) {
			levels[nlevels++] = (int)strtol(s, &s, 10);
			if (*s == ',')
				s++;
		}
		a = 3;
	} else {
		for (int l = 0; l <= 12; l++)
			levels[nlevels++] = l;
	}
	d = libdeflate_alloc_decompressor();
	for (int li = 0; li < nlevels; li++) {
		int level = levels[li];
		struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);

		if (!c || !d) {
			fprintf(stderr, "alloc failed\n");
			return 2;
		}
		for (int f = a; f < argc; f++) {
			FILE *fp = fopen(argv[f], "rb");
			const char *name = strrchr(argv[f], '/');
			const char *bs = strrchr(argv[f], '\\');
			unsigned char *data;
			size_t n;
			char sched[32];

			if (bs && (!name || bs > name))
				name = bs;
			name = name ? name + 1 : argv[f];
			if (!fp) {
				perror(argv[f]);
				return 2;
			}
			fseek(fp, 0, SEEK_END);
			n = (size_t)ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (n > BIG_LIMIT)
				n = BIG_LIMIT;
			data = xmalloc(n);
			if (fread(data, 1, n, fp) != n) {
				perror("fread");
				return 2;
			}
			fclose(fp);
			for (size_t k = 0;
			     k < sizeof(fixed_sizes) / sizeof(fixed_sizes[0]); k++) {
				snprintf(sched, sizeof(sched), "s%zu", fixed_sizes[k]);
				if (run_schedule(c, data, n, name, level, sched,
						 fixed_sizes[k], 0))
					return 1;
			}
			if (run_schedule(c, data, n, name, level, "mixed", 0, 1))
				return 1;
			free(data);
		}
		libdeflate_free_compressor(c);
	}
	libdeflate_free_decompressor(d);
	return 0;
}
