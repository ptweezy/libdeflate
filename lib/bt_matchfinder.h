/*
 * bt_matchfinder.h - Lempel-Ziv matchfinding with a hash table of binary trees
 *
 * Copyright 2016 Eric Biggers
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
 * ----------------------------------------------------------------------------
 *
 * This is a Binary Trees (bt) based matchfinder.
 *
 * The main data structure is a hash table where each hash bucket contains a
 * binary tree of sequences whose first 4 bytes share the same hash code.  Each
 * sequence is identified by its starting position in the input buffer.  Each
 * binary tree is always sorted such that each left child represents a sequence
 * lexicographically lesser than its parent and each right child represents a
 * sequence lexicographically greater than its parent.
 *
 * The algorithm processes the input buffer sequentially.  At each byte
 * position, the hash code of the first 4 bytes of the sequence beginning at
 * that position (the sequence being matched against) is computed.  This
 * identifies the hash bucket to use for that position.  Then, a new binary tree
 * node is created to represent the current sequence.  Then, in a single tree
 * traversal, the hash bucket's binary tree is searched for matches and is
 * re-rooted at the new node.
 *
 * Compared to the simpler algorithm that uses linked lists instead of binary
 * trees (see hc_matchfinder.h), the binary tree version gains more information
 * at each node visitation.  Ideally, the binary tree version will examine only
 * 'log(n)' nodes to find the same matches that the linked list version will
 * find by examining 'n' nodes.  In addition, the binary tree version can
 * examine fewer bytes at each node by taking advantage of the common prefixes
 * that result from the sort order, whereas the linked list version may have to
 * examine up to the full length of the match at each node.
 *
 * However, it is not always best to use the binary tree version.  It requires
 * nearly twice as much memory as the linked list version, and it takes time to
 * keep the binary trees sorted, even at positions where the compressor does not
 * need matches.  Generally, when doing fast compression on small buffers,
 * binary trees are the wrong approach.  They are best suited for thorough
 * compression and/or large buffers.
 *
 * ----------------------------------------------------------------------------
 */

#ifndef LIB_BT_MATCHFINDER_H
#define LIB_BT_MATCHFINDER_H

#include "matchfinder_common.h"

#define BT_MATCHFINDER_HASH3_ORDER 16
#define BT_MATCHFINDER_HASH3_WAYS  2
#define BT_MATCHFINDER_HASH4_ORDER 16

#define BT_MATCHFINDER_TOTAL_HASH_SIZE		\
	(((1UL << BT_MATCHFINDER_HASH3_ORDER) * BT_MATCHFINDER_HASH3_WAYS + \
	  (1UL << BT_MATCHFINDER_HASH4_ORDER)) * sizeof(mf_pos_t))

/* Representation of a match found by the bt_matchfinder  */
struct lz_match {

	/* The number of bytes matched.  */
	u16 length;

	/* The offset back from the current position that was matched.  */
	u16 offset;
};

struct MATCHFINDER_ALIGNED bt_matchfinder {

	/* The hash table for finding length 3 matches  */
	mf_pos_t hash3_tab[1UL << BT_MATCHFINDER_HASH3_ORDER][BT_MATCHFINDER_HASH3_WAYS];

	/* The hash table which contains the roots of the binary trees for
	 * finding length 4+ matches  */
	mf_pos_t hash4_tab[1UL << BT_MATCHFINDER_HASH4_ORDER];

	/* The child node references for the binary trees.  The left and right
	 * children of the node for the sequence with position 'pos' are
	 * 'child_tab[pos * 2]' and 'child_tab[pos * 2 + 1]', respectively.  */
	mf_pos_t child_tab[2UL * MATCHFINDER_WINDOW_SIZE];
};

/* Prepare the matchfinder for a new input buffer.  */
static forceinline void
bt_matchfinder_init(struct bt_matchfinder *mf)
{
	STATIC_ASSERT(BT_MATCHFINDER_TOTAL_HASH_SIZE %
		      MATCHFINDER_SIZE_ALIGNMENT == 0);

	matchfinder_init((mf_pos_t *)mf, BT_MATCHFINDER_TOTAL_HASH_SIZE);
}

static forceinline void
bt_matchfinder_slide_window(struct bt_matchfinder *mf)
{
	STATIC_ASSERT(sizeof(*mf) % MATCHFINDER_SIZE_ALIGNMENT == 0);

	matchfinder_rebase((mf_pos_t *)mf, sizeof(*mf));
}

static forceinline mf_pos_t *
bt_left_child(struct bt_matchfinder *mf, s32 node)
{
	return &mf->child_tab[2 * (node & (MATCHFINDER_WINDOW_SIZE - 1)) + 0];
}

static forceinline mf_pos_t *
bt_right_child(struct bt_matchfinder *mf, s32 node)
{
	return &mf->child_tab[2 * (node & (MATCHFINDER_WINDOW_SIZE - 1)) + 1];
}

/* The minimum permissible value of 'max_len' for bt_matchfinder_get_matches()
 * and bt_matchfinder_skip_byte().  There must be sufficiently many bytes
 * remaining to load a 32-bit integer from the *next* position.  */
#define BT_MATCHFINDER_REQUIRED_NBYTES	5

/*
 * bt_matchfinder_advance_one_byte() is compiled twice from
 * bt_matchfinder_template.h: for inputs in general, and for small inputs.  See
 * "Small inputs" in matchfinder_common.h.
 */
#define SUFFIX		_normal
#define SMALL_INPUT	0
#include "bt_matchfinder_template.h"

#define SUFFIX		_small
#define SMALL_INPUT	1
#include "bt_matchfinder_template.h"

/*
 * Retrieve a list of matches with the current position.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base
 *	Pointer to the next byte in the input buffer to process _at the last
 *	time bt_matchfinder_init() or bt_matchfinder_slide_window() was called_.
 * @cur_pos
 *	The current position in the input buffer relative to @in_base (the
 *	position of the sequence being matched against).
 * @max_len
 *	The maximum permissible match length at this position.  Must be >=
 *	BT_MATCHFINDER_REQUIRED_NBYTES.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 *	Must be <= @max_len.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.  Must be >= 1.
 * @next_hashes
 *	The precomputed hash codes for the sequence beginning at @in_next.
 *	These will be used and then updated with the precomputed hashcodes for
 *	the sequence beginning at @in_next + 1.
 * @lz_matchptr
 *	An array in which this function will record the matches.  The recorded
 *	matches will be sorted by strictly increasing length and (non-strictly)
 *	increasing offset.  The maximum number of matches that may be found is
 *	'nice_len - 2'.
 * @small
 *	Whether the input is small; see "Small inputs" in matchfinder_common.h.
 *	This must be a compile-time constant.
 * @small_base
 *	If @small, the position of the first byte of the input.
 *
 * The return value is a pointer to the next available slot in the @lz_matchptr
 * array.  (If no matches were found, this will be the same as @lz_matchptr.)
 */
static forceinline struct lz_match *
bt_matchfinder_get_matches(struct bt_matchfinder *mf,
			   const u8 *in_base,
			   ptrdiff_t cur_pos,
			   u32 max_len,
			   u32 nice_len,
			   u32 max_search_depth,
			   u32 next_hashes[2],
			   struct lz_match *lz_matchptr,
			   bool small,
			   s32 small_base)
{
	if (small)
		return bt_matchfinder_advance_one_byte_small(
					mf, in_base, cur_pos + small_base,
					max_len, nice_len, max_search_depth,
					next_hashes, lz_matchptr, true,
					small_base);
	return bt_matchfinder_advance_one_byte_normal(mf, in_base, cur_pos,
						      max_len, nice_len,
						      max_search_depth,
						      next_hashes, lz_matchptr,
						      true);
}

/*
 * Advance the matchfinder, but don't record any matches.
 *
 * This is very similar to bt_matchfinder_get_matches() because both functions
 * must do hashing and tree re-rooting.
 */
static forceinline void
bt_matchfinder_skip_byte(struct bt_matchfinder *mf,
			 const u8 *in_base,
			 ptrdiff_t cur_pos,
			 u32 nice_len,
			 u32 max_search_depth,
			 u32 next_hashes[2],
			 bool small,
			 s32 small_base)
{
	if (small)
		bt_matchfinder_advance_one_byte_small(
					mf, in_base, cur_pos + small_base,
					nice_len, nice_len, max_search_depth,
					next_hashes, NULL, false, small_base);
	else
		bt_matchfinder_advance_one_byte_normal(mf, in_base, cur_pos,
						       nice_len, nice_len,
						       max_search_depth,
						       next_hashes, NULL,
						       false);
}

#endif /* LIB_BT_MATCHFINDER_H */
