#!/usr/bin/env python3
"""mlsd_ci prof_patch.py - test only: add per-block timing hooks to a copy of the tree.
usage: prof_patch.py SRCDIR   (SRCDIR has lib/; changed in place)
mlsd_prof_mode 0 keeps the lanes off, 1 lets them run where the gates allow,
2 runs them on every dynamic block they can take (no codespace, long-match or
previous-block gates).  One binary for all three, so that the code is the same."""
import sys, os
src = sys.argv[1]
def edit(path, pairs):
    p = os.path.join(src, path); t = open(p).read()
    for a, b, last in pairs:
        if last:
            i = t.rindex(a); t = t[:i] + b + t[i + len(a):]
        else:
            assert t.count(a) == 1, (path, a); t = t.replace(a, b)
    open(p, 'w').write(t)
edit('lib/decompress_template.h', [
 ("""	mlsd.block_start = (uintptr_t)out_next;
	mlsd_block = MLSD_NO;
#endif
""", """	mlsd.block_start = (uintptr_t)out_next;
	mlsd_block = MLSD_NO;
#endif
	MLSD_PROF_START(out_next, in_next, bitsleft);
""", False),
 ("""#if USE_MLSD
	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN &&
	    mlsd.prev_block_out >= MLSD_MIN_PREV_OUT &&""",
  """	MLSD_PROF_LENS(block_type, d->u.l.lens, num_litlen_syms, num_offset_syms);
#if USE_MLSD
	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN && mlsd_prof_mode &&
	    (mlsd_prof_mode == 2 || mlsd.prev_block_out >= MLSD_MIN_PREV_OUT) &&""", False),
 ("block_done:\n\t/* Finished decoding a block */\n",
  "block_done:\n\t/* Finished decoding a block */\n\tMLSD_PROF_END(out_next, in_next, bitsleft);\n", True),
])
hook = r'''
/* ---- per-block profiling (test only) ---- */
#include <x86intrin.h>
struct mlsd_prof_rec { unsigned long long cyc; unsigned out, inbits, lane_out, lane_rec; unsigned char type, nl, no_, pad; unsigned char lens[320]; };
extern struct mlsd_prof_rec mlsd_prof_recs[]; extern unsigned mlsd_prof_n; extern int mlsd_prof_mode;
extern unsigned long long mlsd_prof_t0; extern const unsigned char *mlsd_prof_o0, *mlsd_prof_i0; extern unsigned mlsd_prof_b0;
#define MLSD_PROF_MAX 200000
#define MLSD_PROF_START(o, i, bl) do { mlsd_prof_o0 = (o); mlsd_prof_i0 = (i); mlsd_prof_b0 = (unsigned char)(bl); \
	mlsd_prof_recs[mlsd_prof_n].lane_out = 0; mlsd_prof_recs[mlsd_prof_n].lane_rec = 0; mlsd_prof_recs[mlsd_prof_n].type = 9; mlsd_prof_t0 = __rdtsc(); } while (0)
#define MLSD_PROF_LENS(bt, l, nl_, no) do { struct mlsd_prof_rec *r_ = &mlsd_prof_recs[mlsd_prof_n]; r_->type = (bt); r_->nl = (nl_) - 257; r_->no_ = (no); \
	if (mlsd_prof_mode == 0 && (bt) == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) { memset(r_->lens, 0, 320); memcpy(r_->lens, (l), (nl_)); memcpy(r_->lens + 288, (l) + (nl_), (no)); } } while (0)
#define MLSD_PROF_END(o, i, bl) do { struct mlsd_prof_rec *r_ = &mlsd_prof_recs[mlsd_prof_n]; unsigned long long t_ = __rdtsc(); \
	r_->cyc = t_ - mlsd_prof_t0; r_->out = (unsigned)((o) - mlsd_prof_o0); \
	r_->inbits = (unsigned)(((i) - mlsd_prof_i0) * 8 - (unsigned char)(bl) + mlsd_prof_b0); if (mlsd_prof_n < MLSD_PROF_MAX - 1) mlsd_prof_n++; } while (0)
'''
p = os.path.join(src, 'lib/deflate_decompress.c'); c = open(p).read()
i = c.index('#include "lib_common.h"'); j = c.index('\n', i) + 1
open(p, 'w').write(c[:j] + hook + c[j:])
edit('lib/decompress_multilane.h', [
 ("""	state->rate16 = rate16 - 12 * 16;
	state->margin = margin - 256;
""", """	state->rate16 = rate16 - 12 * 16;
	state->margin = margin - 256;
	mlsd_prof_recs[mlsd_prof_n].lane_out = (unsigned)(out - ctx->out_next);
	mlsd_prof_recs[mlsd_prof_n].lane_rec = (unsigned)nrec;
""", False),
 ("if (len_space < MLSD_MIN_LEN_SPACE ||",
  "if ((mlsd_prof_mode == 1 && len_space < MLSD_MIN_LEN_SPACE) ||", False),
])
