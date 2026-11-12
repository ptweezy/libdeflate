/*
 * ht_matchfinder.h - Lempel-Ziv matchfinding with a hash table
 *
 * Copyright 2022 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 *
 * This is a Hash Table (ht) matchfinder.
 *
 * This is a variant of the Hash Chains (hc) matchfinder that is optimized for
 * very fast compression.  The ht_matchfinder stores the hash chains inline in
 * the hash table, whereas the hc_matchfinder stores them in a separate array.
 * Storing the hash chains inline is the faster method when max_search_depth
 * (the maximum chain length) is very small.  It is not appropriate when
 * max_search_depth is larger, as then it uses too much memory.
 *
 * Due to its focus on speed, the ht_matchfinder doesn't support length 3
 * matches.  It also doesn't allow max_search_depth to vary at runtime; it is
 * fixed at build time as HT_MATCHFINDER_BUCKET_SIZE.
 *
 * See hc_matchfinder.h for more information.
 */

#ifndef LIB_HT_MATCHFINDER_H
#define LIB_HT_MATCHFINDER_H

#include "matchfinder_common.h"

#define HT_MATCHFINDER_HASH_ORDER	15
#define HT_MATCHFINDER_BUCKET_SIZE	2

#define HT_MATCHFINDER_MIN_MATCH_LEN	4
/* Minimum value of max_len for ht_matchfinder_longest_match() */
#define HT_MATCHFINDER_REQUIRED_NBYTES	5

struct MATCHFINDER_ALIGNED ht_matchfinder {
	mf_pos_t hash_tab[1UL << HT_MATCHFINDER_HASH_ORDER]
			 [HT_MATCHFINDER_BUCKET_SIZE];
};

static forceinline void
ht_matchfinder_init(struct ht_matchfinder *mf)
{
	STATIC_ASSERT(sizeof(*mf) % MATCHFINDER_SIZE_ALIGNMENT == 0);

	matchfinder_init((mf_pos_t *)mf, sizeof(*mf));
}

static forceinline void
ht_matchfinder_slide_window(struct ht_matchfinder *mf)
{
	matchfinder_rebase((mf_pos_t *)mf, sizeof(*mf));
}

/*
 * ht_matchfinder_longest_match() and ht_matchfinder_skip_bytes() are compiled
 * twice from ht_matchfinder_template.h: for inputs in general, and for small
 * inputs.  See "Small inputs" in matchfinder_common.h.
 */
#define SUFFIX		_normal
#define SMALL_INPUT	0
#include "ht_matchfinder_template.h"

#define SUFFIX		_small
#define SMALL_INPUT	1
#include "ht_matchfinder_template.h"

/*
 * Note: max_len must be >= HT_MATCHFINDER_REQUIRED_NBYTES.  @small is whether
 * the input is small, and it must be a compile-time constant.  If @small,
 * @small_base is the position of the first byte of the input.
 */
static forceinline u32
ht_matchfinder_longest_match(struct ht_matchfinder * const mf,
			     const u8 ** const in_base_p,
			     const u8 * const in_next,
			     const u32 max_len,
			     const u32 nice_len,
			     u32 * const next_hash,
			     u32 * const offset_ret,
			     const bool small,
			     const s32 small_base)
{
	if (small)
		return ht_matchfinder_longest_match_small(mf, in_base_p,
							  in_next, max_len,
							  nice_len, next_hash,
							  offset_ret,
							  small_base);
	return ht_matchfinder_longest_match_normal(mf, in_base_p, in_next,
						   max_len, nice_len, next_hash,
						   offset_ret);
}

static forceinline void
ht_matchfinder_skip_bytes(struct ht_matchfinder * const mf,
			  const u8 ** const in_base_p,
			  const u8 *in_next,
			  const u8 * const in_end,
			  const u32 count,
			  u32 * const next_hash,
			  const bool small,
			  const s32 small_base)
{
	if (small)
		ht_matchfinder_skip_bytes_small(mf, in_base_p, in_next, in_end,
						count, next_hash, small_base);
	else
		ht_matchfinder_skip_bytes_normal(mf, in_base_p, in_next, in_end,
						 count, next_hash);
}

#endif /* LIB_HT_MATCHFINDER_H */
