/*
 * Differential fuzz: A (baseline) vs B (multi-lane forced) on valid and
 * corrupted large streams.  The compressed input and both output buffers end
 * exactly at an unmapped guard page, so any read past the end of the input
 * (including by vector gathers, which ASan does not check) or write past the
 * end of the output faults.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "libdeflate.h"
#define DECL(p) struct libdeflate_decompressor *p##_alloc_decompressor(void); \
enum libdeflate_result p##_deflate_decompress_ex(struct libdeflate_decompressor *, const void *, size_t, void *, size_t, size_t *, size_t *);
DECL(A) DECL(B)
/* A buffer of at least 'cap' bytes whose end is followed by an unmapped page. */
static unsigned char *guarded_end(size_t cap) {
	size_t pg = (size_t)sysconf(_SC_PAGESIZE), sz = (cap + pg - 1) / pg * pg;
	unsigned char *p = mmap(NULL, sz + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || mprotect(p + sz, pg, PROT_NONE) != 0) { perror("guard page"); exit(2); }
	return p + sz;
}
static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
int main(int argc, char **argv) {
	int iters = atoi(argv[1]);
	rs ^= strtoull(argv[2], 0, 0);
	/* load corpus files */
	int nf = argc - 3; unsigned char *data[16]; size_t dlen[16];
	for (int i = 0; i < nf; i++) { FILE *fp = fopen(argv[i + 3], "rb"); fseek(fp, 0, SEEK_END); dlen[i] = ftell(fp); fseek(fp, 0, SEEK_SET); data[i] = malloc(dlen[i]); fread(data[i], 1, dlen[i], fp); fclose(fp); }
	struct libdeflate_decompressor *da = A_alloc_decompressor(), *db = B_alloc_decompressor();
	struct libdeflate_compressor *cs[13]; for (int l = 0; l <= 12; l++) cs[l] = libdeflate_alloc_compressor(l);
	size_t maxn = 3 << 20;
	unsigned char *in = malloc(maxn), *cbuf = malloc(maxn * 2);
	unsigned char *gin = guarded_end(maxn * 2), *goa = guarded_end(maxn + 4096), *gob = guarded_end(maxn + 4096);
	long nvalid = 0, ncorrupt = 0, nsucc = 0, ndiff = 0;
	for (int it = 0; it < iters; it++) {
		size_t n = 200000 + rnd() % (maxn - 200000);
		int f = rnd() % (nf + 1);
		if (f == nf) { for (size_t i = 0; i < n; i++) in[i] = (rnd() % 7 == 0) ? rnd() : (i ? in[i - 1 - rnd() % (i < 300 ? i : 300)] : 0); }
		else { if (n > dlen[f]) n = dlen[f]; size_t off = rnd() % (dlen[f] - n + 1); memcpy(in, data[f] + off, n); }
		int level = rnd() % 13;
		size_t cl = libdeflate_deflate_compress(cs[level], in, n, cbuf, maxn * 2);
		if (!cl) continue;
		for (int t = 0; t < 6; t++) {
			size_t clen = cl;
			unsigned char *c = cbuf;
			static unsigned char tmp[6 << 20];
			memcpy(tmp, cbuf, cl);
			int kind = t == 0 ? 0 : 1 + rnd() % 3;
			if (kind == 1) { int k = 1 + rnd() % 8; for (int q = 0; q < k; q++) { size_t pos = rnd() % cl; tmp[pos] ^= 1 << (rnd() % 8); } }
			else if (kind == 2) { size_t pos = rnd() % cl, len = 1 + rnd() % 64; for (size_t q = pos; q < pos + len && q < cl; q++) tmp[q] = rnd(); }
			else if (kind == 3) { clen = 1 + rnd() % cl; }
			c = gin - clen;
			memcpy(c, tmp, clen);
			size_t outsz = n; int os = rnd() % 4;
			if (os == 1) outsz = n - rnd() % (n / 4 + 1); else if (os == 2) outsz = n + rnd() % 4096;
			unsigned char *oa = goa - outsz, *ob = gob - outsz;
			size_t ia = 0, ib = 0, xa = 0, xb = 0;
			int useex = rnd() % 2;
			memset(oa, 0, outsz); memset(ob, 0, outsz);
			enum libdeflate_result ra = A_deflate_decompress_ex(da, c, clen, oa, outsz, useex ? &ia : NULL, useex ? &xa : NULL);
			enum libdeflate_result rb = B_deflate_decompress_ex(db, c, clen, ob, outsz, useex ? &ib : NULL, useex ? &xb : NULL);
			if (kind == 0) nvalid++; else ncorrupt++;
			if (ra == LIBDEFLATE_SUCCESS) nsucc++;
			int bad = ra != rb;
			if (!bad && ra == LIBDEFLATE_SUCCESS) bad = (useex && (ia != ib || xa != xb)) || memcmp(oa, ob, useex ? xa : outsz);
			if (bad) { ndiff++; printf("DIFF it=%d t=%d kind=%d level=%d n=%zu outsz=%zu ra=%d rb=%d ia=%zu ib=%zu xa=%zu xb=%zu\n", it, t, kind, level, n, outsz, ra, rb, ia, ib, xa, xb); if (ndiff > 10) { printf("STOPPED EARLY valid %ld corrupt %ld success %ld guard-pages diffs %ld\n", nvalid, ncorrupt, nsucc, ndiff); return 1; } }
		}
	}
	printf("valid %ld corrupt %ld success %ld guard-pages diffs %ld\n", nvalid, ncorrupt, nsucc, ndiff);
	return ndiff != 0;
}
