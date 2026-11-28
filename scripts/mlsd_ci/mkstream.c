/* mkstream LEVEL CHUNK FILE OUT: compress FILE in CHUNK-byte pieces (0 = whole) into an "LDST" stream file */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libdeflate.h"
int main(int argc, char **argv)
{
	int level = atoi(argv[1]); size_t chunk = strtoul(argv[2], 0, 10), n, pos; uint32_t nchunks;
	FILE *f = fopen(argv[3], "rb"), *o; unsigned char *in, *out;
	struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	in = malloc(n); if (fread(in, 1, n, f) != n) return 1;
	if (chunk == 0) chunk = n;
	out = malloc(chunk + chunk / 8 + 4096);
	nchunks = (n + chunk - 1) / chunk;
	o = fopen(argv[4], "wb"); fwrite("LDST", 1, 4, o); fwrite(&nchunks, 4, 1, o);
	for (pos = 0; pos < n; pos += chunk) {
		uint32_t u = n - pos < chunk ? n - pos : chunk;
		uint32_t z = libdeflate_deflate_compress(c, in + pos, u, out, chunk + chunk / 8 + 4096);
		fwrite(&u, 4, 1, o); fwrite(&z, 4, 1, o); fwrite(out, 1, z, o);
	}
	fclose(o);
	return 0;
}
