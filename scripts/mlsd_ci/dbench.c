/*
 * dbench -n TRIALS [-B BUFSIZE] STREAMFILE ORIGFILE
 * Times decompression of an "LDST" stream file (u32 magic, u32 nchunks, then
 * per chunk: u32 out size, u32 in size, data), each chunk into its own
 * exact-size (or BUFSIZE, if larger) output buffer.  Prints the fastest total in ns.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libdeflate.h"

static uint64_t now_ns(void)
{
	struct timespec ts;
#ifdef __APPLE__
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}
static unsigned char *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb"); unsigned char *p;
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	p = malloc(*n + 64); if (fread(p, 1, *n, f) != *n) exit(2);
	fclose(f); return p;
}
int main(int argc, char **argv)
{
	int trials = 5, a = 1, t; size_t bufsize = 0, sn, on, total = 0, i;
	unsigned char *s, *orig, *out; uint32_t nchunks, c; uint64_t best = UINT64_MAX;
	struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();

	while (a + 1 < argc && argv[a][0] == '-') {
		if (argv[a][1] == 'n') trials = atoi(argv[a + 1]);
		else if (argv[a][1] == 'B') bufsize = strtoul(argv[a + 1], NULL, 10);
		a += 2;
	}
	if (argc - a < 2) return 2;
	s = slurp(argv[a], &sn); orig = slurp(argv[a + 1], &on);
	if (memcmp(s, "LDST", 4)) { fprintf(stderr, "bad magic\n"); return 2; }
	memcpy(&nchunks, s + 4, 4);
	for (i = 8, c = 0; c < nchunks; c++) { uint32_t u, z; memcpy(&u, s + i, 4); memcpy(&z, s + i + 4, 4); total += u; i += 8 + z; }
	if (i != sn || total != on) { fprintf(stderr, "bad stream file %zu %zu %zu %zu\n", i, sn, total, on); return 2; }
	/* every chunk gets its own buffer, as separate streams would */
	out = malloc(total + (bufsize ? (size_t)nchunks * bufsize : 0) + 64);
	memset(out, 1, total + (bufsize ? (size_t)nchunks * bufsize : 0) + 64);
	for (t = 0; t <= trials; t++) {
		uint64_t sum = 0; size_t opos = 0;
		for (i = 8, c = 0; c < nchunks; c++) {
			uint32_t u, z; size_t actual, avail; uint64_t t0; enum libdeflate_result r;
			memcpy(&u, s + i, 4); memcpy(&z, s + i + 4, 4);
			avail = bufsize > u ? bufsize : u;
			t0 = now_ns();
			r = libdeflate_deflate_decompress(d, s + i + 8, z, out + opos, avail, &actual);
			sum += now_ns() - t0;
			if (r != LIBDEFLATE_SUCCESS || actual != u) { fprintf(stderr, "decompress fail %d\n", r); return 1; }
			if (t == 0 && memcmp(out + opos, orig + (total ? 0 : 0) + (opos - (bufsize ? 0 : 0)) * 0 + 0, 0)) return 1;
			i += 8 + z; opos += avail;
		}
		if (t > 0 && sum < best) best = sum;
	}
	/* verify */
	{ size_t opos = 0, ipos = 0;
	  for (i = 8, c = 0; c < nchunks; c++) { uint32_t u, z; memcpy(&u, s + i, 4); memcpy(&z, s + i + 4, 4);
		if (memcmp(out + opos, orig + ipos, u)) { fprintf(stderr, "MISMATCH %s chunk %u\n", argv[a], c); return 1; }
		opos += bufsize > u ? bufsize : u; ipos += u; i += 8 + z; } }
	printf("%llu\n", (unsigned long long)best);
	return 0;
}
