/*
 * prof [-n TRIALS] STREAMFILE OUT.csv OUT.lens
 * Per-block cycles of a single-chunk LDST stream with the lanes off (mode 0)
 * and on (mode 1), minimum over TRIALS alternating runs.  Needs the src-prof
 * library (MLSD_PROF hooks).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libdeflate.h"
struct mlsd_prof_rec { unsigned long long cyc; unsigned out, inbits, lane_out, lane_rec; unsigned char type, nl, no_, pad; unsigned char lens[320]; };
#define MAXB 200000
struct mlsd_prof_rec mlsd_prof_recs[MAXB]; unsigned mlsd_prof_n; int mlsd_prof_mode;
unsigned long long mlsd_prof_t0; const unsigned char *mlsd_prof_o0, *mlsd_prof_i0; unsigned mlsd_prof_b0;
static struct mlsd_prof_rec best[2][MAXB];
int main(int argc, char **argv)
{
	int trials = 15, a = 1, t, m; size_t sn; FILE *f; unsigned char *s, *out; uint32_t nchunks, u, z; unsigned nb[2] = {0, 0}, i;
	struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
	if (argc > 2 && !strcmp(argv[1], "-n")) { trials = atoi(argv[2]); a = 3; }
	if (argc - a < 3) return 2;
	f = fopen(argv[a], "rb"); if (!f) return 2; fseek(f, 0, SEEK_END); sn = ftell(f); fseek(f, 0, SEEK_SET);
	s = malloc(sn + 64); if (fread(s, 1, sn, f) != sn) return 2; fclose(f);
	memcpy(&nchunks, s + 4, 4); if (memcmp(s, "LDST", 4) || nchunks != 1) { fprintf(stderr, "need a 1-chunk stream\n"); return 2; }
	memcpy(&u, s + 8, 4); memcpy(&z, s + 12, 4);
	out = malloc(u + 64); memset(out, 1, u + 64);
	for (t = 0; t < trials; t++) for (m = 0; m < 2; m++) {
		size_t actual;
		mlsd_prof_mode = m; mlsd_prof_n = 0;
		if (libdeflate_deflate_decompress(d, s + 16, z, out, u, &actual) != LIBDEFLATE_SUCCESS || actual != u) { fprintf(stderr, "fail\n"); return 1; }
		if (t == 0) { nb[m] = mlsd_prof_n; memcpy(best[m], mlsd_prof_recs, sizeof(best[m][0]) * mlsd_prof_n); }
		else { if (mlsd_prof_n != nb[m]) { fprintf(stderr, "block count changed\n"); return 1; }
			for (i = 0; i < mlsd_prof_n; i++) if (mlsd_prof_recs[i].cyc < best[m][i].cyc) { unsigned char l[320]; memcpy(l, best[m][i].lens, 320); best[m][i] = mlsd_prof_recs[i]; if (m == 0) memcpy(best[m][i].lens, l, 320); } }
	}
	if (nb[0] != nb[1]) { fprintf(stderr, "block counts differ\n"); return 1; }
	f = fopen(argv[a + 1], "w"); fprintf(f, "idx,type,out,inbits,nl,no,cyc_fast,cyc_lanes,lane_out,lane_rec\n");
	for (i = 0; i < nb[0]; i++) fprintf(f, "%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%u\n", i, best[0][i].type, best[0][i].out, best[0][i].inbits, best[0][i].nl + 257, best[0][i].no_, best[0][i].cyc, best[1][i].cyc, best[1][i].lane_out, best[1][i].lane_rec);
	fclose(f);
	f = fopen(argv[a + 2], "wb"); for (i = 0; i < nb[0]; i++) fwrite(best[0][i].lens, 1, 320, f); fclose(f);
	return 0;
}
