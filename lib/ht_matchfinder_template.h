/*
 * ht_matchfinder_template.h - template for the ht_matchfinder search functions
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
 */

/*
 * This file is a template parameterized by SUFFIX and SMALL_INPUT.
 * ht_matchfinder.h includes it twice with different "arguments", so it
 * intentionally has no include guard.
 *
 * It implements ht_matchfinder_longest_match() and ht_matchfinder_skip_bytes().
 *
 * SUFFIX:
 *	Name suffix to append to all instantiated functions.
 * SMALL_INPUT:
 *	If 0, instantiate the functions for inputs in general.
 *	If 1, instantiate the functions for small inputs, which use a map in
 *	place of each hash table; see "Small inputs" in matchfinder_common.h.
 */

/*
 * SMALL_ORDER_PARAM is the additional parameter of the functions for small
 * inputs: the order of the map.  BUCKET is the bucket of 'hash'.
 */
#if SMALL_INPUT
#  define SMALL_ORDER_PARAM	, const unsigned order
#  define BUCKET		bucket
#else
#  define SMALL_ORDER_PARAM
#  define BUCKET		mf->hash_tab[hash]
#endif

/* Note: max_len must be >= HT_MATCHFINDER_REQUIRED_NBYTES */
static forceinline u32
ADD_SUFFIX(ht_matchfinder_longest_match)(struct ht_matchfinder * const mf,
					 const u8 ** const in_base_p,
					 const u8 * const in_next,
					 const u32 max_len,
					 const u32 nice_len,
					 u32 * const next_hash,
					 u32 * const offset_ret
					 SMALL_ORDER_PARAM)
{
	u32 best_len = 0;
	const u8 *best_matchptr = in_next;
	u32 cur_pos = in_next - *in_base_p;
	const u8 *in_base;
	mf_pos_t cutoff;
	u32 hash;
	u32 seq;
	mf_pos_t cur_node;
#if SMALL_INPUT
	mf_pos_t *bucket;
#endif
	const u8 *matchptr;
#if HT_MATCHFINDER_BUCKET_SIZE > 1
	mf_pos_t to_insert;
	u32 len;
#endif
#if HT_MATCHFINDER_BUCKET_SIZE > 2
	int i;
#endif

	/* This is assumed throughout this function. */
	STATIC_ASSERT(HT_MATCHFINDER_MIN_MATCH_LEN == 4);

#if !SMALL_INPUT
	if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
		ht_matchfinder_slide_window(mf);
		*in_base_p += MATCHFINDER_WINDOW_SIZE;
		cur_pos = 0;
	}
#endif
	in_base = *in_base_p;
	cutoff = cur_pos - MATCHFINDER_WINDOW_SIZE;

	hash = *next_hash;
	STATIC_ASSERT(HT_MATCHFINDER_REQUIRED_NBYTES == 5);
	*next_hash = lz_hash(get_unaligned_le32(in_next + 1),
			     HT_MATCHFINDER_HASH_ORDER);
	seq = load_u32_unaligned(in_next);
#if SMALL_INPUT
	bucket = matchfinder_small_bucket((mf_pos_t *)mf,
					  HT_MATCHFINDER_SLOT_SIZE, hash,
					  HT_MATCHFINDER_HASH_ORDER, order);
#else
	prefetchw(&mf->hash_tab[*next_hash]);
#endif
#if HT_MATCHFINDER_BUCKET_SIZE == 1
	/* Hand-unrolled version for BUCKET_SIZE == 1 */
	cur_node = BUCKET[0];
	BUCKET[0] = cur_pos;
	if (cur_node <= cutoff)
		goto out;
	matchptr = &in_base[cur_node];
	if (load_u32_unaligned(matchptr) == seq) {
		best_len = lz_extend(in_next, matchptr, 4, max_len);
		best_matchptr = matchptr;
	}
#elif HT_MATCHFINDER_BUCKET_SIZE == 2
	/*
	 * Hand-unrolled version for BUCKET_SIZE == 2.  The logic here also
	 * differs slightly in that it copies the first entry to the second even
	 * if nice_len is reached on the first, as this can be slightly faster.
	 */
	cur_node = BUCKET[0];
	BUCKET[0] = cur_pos;
	if (cur_node <= cutoff)
		goto out;
	matchptr = &in_base[cur_node];

	to_insert = cur_node;
	cur_node = BUCKET[1];
	BUCKET[1] = to_insert;

	if (load_u32_unaligned(matchptr) == seq) {
		best_len = lz_extend(in_next, matchptr, 4, max_len);
		best_matchptr = matchptr;
		if (cur_node <= cutoff || best_len >= nice_len)
			goto out;
		matchptr = &in_base[cur_node];
		if (load_u32_unaligned(matchptr) == seq &&
		    load_u32_unaligned(matchptr + best_len - 3) ==
		    load_u32_unaligned(in_next + best_len - 3)) {
			len = lz_extend(in_next, matchptr, 4, max_len);
			if (len > best_len) {
				best_len = len;
				best_matchptr = matchptr;
			}
		}
	} else {
		if (cur_node <= cutoff)
			goto out;
		matchptr = &in_base[cur_node];
		if (load_u32_unaligned(matchptr) == seq) {
			best_len = lz_extend(in_next, matchptr, 4, max_len);
			best_matchptr = matchptr;
		}
	}
#else
	/* Generic version for HT_MATCHFINDER_BUCKET_SIZE > 2 */
	to_insert = cur_pos;
	for (i = 0; i < HT_MATCHFINDER_BUCKET_SIZE; i++) {
		cur_node = BUCKET[i];
		BUCKET[i] = to_insert;
		if (cur_node <= cutoff)
			goto out;
		matchptr = &in_base[cur_node];
		if (load_u32_unaligned(matchptr) == seq) {
			len = lz_extend(in_next, matchptr, 4, max_len);
			if (len > best_len) {
				best_len = len;
				best_matchptr = matchptr;
				if (best_len >= nice_len)
					goto out;
			}
		}
		to_insert = cur_node;
	}
#endif
out:
	*offset_ret = in_next - best_matchptr;
	return best_len;
}

static forceinline void
ADD_SUFFIX(ht_matchfinder_skip_bytes)(struct ht_matchfinder * const mf,
				      const u8 ** const in_base_p,
				      const u8 *in_next,
				      const u8 * const in_end,
				      const u32 count,
				      u32 * const next_hash
				      SMALL_ORDER_PARAM)
{
	s32 cur_pos = in_next - *in_base_p;
	u32 hash;
	u32 remaining = count;
#if SMALL_INPUT
	mf_pos_t *bucket;
#endif
	int i;

	if (unlikely(count + HT_MATCHFINDER_REQUIRED_NBYTES > in_end - in_next))
		return;

#if !SMALL_INPUT
	if (cur_pos + count - 1 >= MATCHFINDER_WINDOW_SIZE) {
		ht_matchfinder_slide_window(mf);
		*in_base_p += MATCHFINDER_WINDOW_SIZE;
		cur_pos -= MATCHFINDER_WINDOW_SIZE;
	}
#endif

	hash = *next_hash;
	do {
#if SMALL_INPUT
		bucket = matchfinder_small_bucket((mf_pos_t *)mf,
						  HT_MATCHFINDER_SLOT_SIZE, hash,
						  HT_MATCHFINDER_HASH_ORDER, order);
#endif
		for (i = HT_MATCHFINDER_BUCKET_SIZE - 1; i > 0; i--)
			BUCKET[i] = BUCKET[i - 1];
		BUCKET[0] = cur_pos;

		hash = lz_hash(get_unaligned_le32(++in_next),
			       HT_MATCHFINDER_HASH_ORDER);
		cur_pos++;
	} while (--remaining);

#if !SMALL_INPUT
	prefetchw(&mf->hash_tab[hash]);
#endif
	*next_hash = hash;
}

#undef SMALL_ORDER_PARAM
#undef BUCKET

#undef SUFFIX
#undef SMALL_INPUT
