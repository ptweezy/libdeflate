/*
 * r6f4v identity driver: one compressor per level, reused for NCALLS calls
 * with random inputs (corpus slices + in-process synthetic data with
 * controlled alphabet sizes and matches), random wrapper format, and output
 * buffers of size bound, exact, exact-1, random < exact, and tiny.  For every
 * call, prints the return value and an FNV-1a hash of the WHOLE output buffer
 * (avail bytes) plus a 64-byte guard zone after it, which must be untouched
 * (h), and a hash of just the returned compressed bytes (o), which doesn't
 * depend on how the library writes into buffers.
 *
 * usage: vid SEED NCALLS "LEVELS" FILE...
 * env:   GARB=n  fill every allocation made by libdeflate with PRNG bytes
 *                seeded by n (default 0 = plain malloc, no fill)
 *        MAXLEN  maximum input length (default 524288)
 *
 * Adapted for CI from handoff/tools/identity/vid.c: the o hash.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libdeflate.h"

static uint64_t rs;
static uint64_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return rs;
}
static uint64_t rnd_n(uint64_t n) { return n ? rnd() % n : 0; }

static uint64_t garb_state;
static void *gmalloc(size_t n)
{
	unsigned char *p = malloc(n);
	size_t i;

	if (p && garb_state) {
		uint64_t s = garb_state;

		for (i = 0; i < n; i++) {
			s ^= s << 13; s ^= s >> 7; s ^= s << 17;
			p[i] = (unsigned char)s;
		}
		garb_state = s;
	}
	return p;
}

static uint64_t fnv(const unsigned char *p, size_t n, uint64_t h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

struct file { unsigned char *d; size_t n; };

static unsigned char *gen_synth(size_t len, unsigned char *buf)
{
	static const unsigned ks[] = { 1, 2, 3, 4, 5, 8, 13, 16, 19, 20, 21,
		22, 23, 31, 32, 64, 100, 128, 192, 200, 210, 213, 214, 215,
		216, 217, 218, 220, 230, 250, 255, 256 };
	unsigned k = ks[rnd_n(sizeof(ks) / sizeof(ks[0]))];
	unsigned base = (unsigned)rnd_n(257 - k);
	unsigned mode = (unsigned)rnd_n(4);
	unsigned match_rate = (unsigned)rnd_n(4) == 0 ? 0 :
			      (unsigned)rnd_n(64);   /* per 64 positions */
	unsigned maxdist = 1u << (1 + rnd_n(15));
	size_t i = 0;

	while (i < len) {
		if (i > 0 && rnd_n(64) < match_rate) {
			size_t d = 1 + rnd_n(i < maxdist ? i : maxdist);
			size_t l = 3 + (rnd_n(4) ? rnd_n(16) : rnd_n(256));
			size_t j;

			for (j = 0; j < l && i < len; j++, i++)
				buf[i] = buf[i - d];
			continue;
		}
		switch (mode) {
		case 0: buf[i] = (unsigned char)(base + rnd_n(k)); break;
		case 1: { /* skewed: geometric-ish */
			unsigned v = 0;
			while (v + 1 < k && (rnd() & 3) == 0)
				v++;
			buf[i] = (unsigned char)(base + v);
			break; }
		case 2: /* each symbol at least once early, then uniform */
			buf[i] = (unsigned char)(base + (i < k ? i : rnd_n(k)));
			break;
		default: /* runs */
			buf[i] = (unsigned char)(base + rnd_n(k));
			{
				size_t r = rnd_n(20), j;
				for (j = 0; j < r && i + 1 < len; j++) {
					i++;
					buf[i] = buf[i - 1];
				}
			}
			break;
		}
		i++;
	}
	return buf;
}

int main(int argc, char **argv)
{
	unsigned long seed, ncalls;
	const char *levels;
	struct file *files;
	int nfiles, i;
	size_t maxlen = getenv("MAXLEN") ? strtoul(getenv("MAXLEN"), 0, 0)
					  : 524288;
	unsigned char *synth;
	struct libdeflate_options opts;
	char lvbuf[256], *tok;

	if (argc < 5) {
		fprintf(stderr, "usage: vid SEED NCALLS LEVELS FILE...\n");
		return 2;
	}
	seed = strtoul(argv[1], 0, 0);
	ncalls = strtoul(argv[2], 0, 0);
	levels = argv[3];
	garb_state = getenv("GARB") ? strtoull(getenv("GARB"), 0, 0) : 0;
	nfiles = argc - 4;
	files = calloc(nfiles, sizeof(files[0]));
	for (i = 0; i < nfiles; i++) {
		FILE *f = fopen(argv[4 + i], "rb");
		if (!f) { perror(argv[4 + i]); return 1; }
		files[i].d = malloc(4 << 20);
		files[i].n = fread(files[i].d, 1, 4 << 20, f);
		fclose(f);
	}
	synth = malloc(maxlen + 1);
	memset(&opts, 0, sizeof(opts));
	opts.sizeof_options = sizeof(opts);
	opts.malloc_func = gmalloc;
	opts.free_func = free;

	snprintf(lvbuf, sizeof(lvbuf), "%s", levels);
	for (tok = strtok(lvbuf, " ,"); tok; tok = strtok(NULL, " ,")) {
		int level = atoi(tok);
		struct libdeflate_compressor *c =
			libdeflate_alloc_compressor_ex(level, &opts);
		uint64_t lh = 0xcbf29ce484222325ULL;
		unsigned long call;

		if (!c) { fprintf(stderr, "alloc failed\n"); return 1; }
		rs = seed * 0x9E3779B97F4A7C15ULL + (uint64_t)level * 7919 + 1;
		for (call = 0; call < ncalls; call++) {
			const unsigned char *in;
			size_t len, bound, avail[6];
			int navail = 0, a, fmt;
			unsigned char *out;
			size_t r0 = 0;
			uint64_t pick = rnd_n(100);

			if (pick < 10)
				len = rnd_n(65);
			else if (pick < 60)
				len = 65 + rnd_n(4096 - 65);
			else if (pick < 90)
				len = 4096 + rnd_n(65536 - 4096);
			else
				len = 65536 + rnd_n(maxlen - 65536);
			if (rnd_n(10) < 3) {
				in = gen_synth(len, synth);
			} else {
				struct file *f = &files[rnd_n(nfiles)];
				size_t off;

				if (len > f->n)
					len = f->n;
				off = rnd_n(f->n - len + 1);
				in = f->d + off;
			}
			pick = rnd_n(10);
			fmt = pick < 8 ? 0 : pick < 9 ? 1 : 2;
			bound = fmt == 0 ? libdeflate_deflate_compress_bound(c, len)
			      : fmt == 1 ? libdeflate_zlib_compress_bound(c, len)
			      : libdeflate_gzip_compress_bound(c, len);
			out = malloc(bound + 64);
			avail[navail++] = bound;
			for (a = 0; a < 6; a++) {
				size_t av, r;
				uint64_t h, o;

				if (a == navail) {
					/* choose more sizes after the first call */
					if (a == 1 && r0 > 0) {
						avail[navail++] = r0;
						avail[navail++] = r0 - 1;
						if (rnd_n(2))
							avail[navail++] = rnd_n(r0);
						if (rnd_n(8) == 0)
							avail[navail++] = rnd_n(17);
					}
					if (a == navail)
						break;
				}
				av = avail[a];
				memset(out, (int)(0xA5 ^ a ^ (call & 0xff)),
				       bound + 64);
				if (fmt == 0)
					r = libdeflate_deflate_compress(c, in, len, out, av);
				else if (fmt == 1)
					r = libdeflate_zlib_compress(c, in, len, out, av);
				else
					r = libdeflate_gzip_compress(c, in, len, out, av);
				if (a == 0)
					r0 = r;
				h = fnv(out, av + 64 <= bound + 64 ? av + 64 : bound + 64,
					0xcbf29ce484222325ULL);
				o = fnv(out, r <= av ? r : 0, 0xcbf29ce484222325ULL);
				{
					size_t g;
					for (g = av; g < av + 64 && g < bound + 64; g++)
						if (out[g] != (unsigned char)(0xA5 ^ a ^ (call & 0xff))) {
							printf("GUARD OVERWRITE L%d call %lu avail %zu at %zu\n",
							       level, call, av, g);
							break;
						}
				}
				printf("L%d c%lu f%d n%zu av%zu r%zu o%016llx h%016llx\n",
				       level, call, fmt, len, av, r,
				       (unsigned long long)o, (unsigned long long)h);
				lh = (lh ^ h ^ r) * 0x100000001b3ULL;
			}
			free(out);
		}
		printf("LEVEL %d calls %lu digest %016llx\n", level, ncalls,
		       (unsigned long long)lh);
		libdeflate_free_compressor(c);
	}
	return 0;
}
