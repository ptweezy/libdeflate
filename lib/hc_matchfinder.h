/*
 * hc_matchfinder.h - Lempel-Ziv matchfinding with a hash table of linked lists
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
 * ---------------------------------------------------------------------------
 *
 *				   Algorithm
 *
 * This is a Hash Chains (hc) based matchfinder.
 *
 * The main data structure is a hash table where each hash bucket contains a
 * linked list (or "chain") of sequences whose first 4 bytes share the same hash
 * code.  Each sequence is identified by its starting position in the input
 * buffer.
 *
 * The algorithm processes the input buffer sequentially.  At each byte
 * position, the hash code of the first 4 bytes of the sequence beginning at
 * that position (the sequence being matched against) is computed.  This
 * identifies the hash bucket to use for that position.  Then, this hash
 * bucket's linked list is searched for matches.  Then, a new linked list node
 * is created to represent the current sequence and is prepended to the list.
 *
 * This algorithm has several useful properties:
 *
 * - It only finds true Lempel-Ziv matches; i.e., those where the matching
 *   sequence occurs prior to the sequence being matched against.
 *
 * - The sequences in each linked list are always sorted by decreasing starting
 *   position.  Therefore, the closest (smallest offset) matches are found
 *   first, which in many compression formats tend to be the cheapest to encode.
 *
 * - Although fast running time is not guaranteed due to the possibility of the
 *   lists getting very long, the worst degenerate behavior can be easily
 *   prevented by capping the number of nodes searched at each position.
 *
 * - If the compressor decides not to search for matches at a certain position,
 *   then that position can be quickly inserted without searching the list.
 *
 * - The algorithm is adaptable to sliding windows: just store the positions
 *   relative to a "base" value that is updated from time to time, and stop
 *   searching each list when the sequences get too far away.
 *
 * ----------------------------------------------------------------------------
 *
 *				 Optimizations
 *
 * The main hash table and chains handle length 4+ matches.  Length 3 matches
 * are handled by a separate hash table with no chains.  This works well for
 * typical "greedy" or "lazy"-style compressors, where length 3 matches are
 * often only helpful if they have small offsets.  Instead of searching a full
 * chain for length 3+ matches, the algorithm just checks for one close length 3
 * match, then focuses on finding length 4+ matches.
 *
 * The longest_match() and skip_bytes() functions are inlined into the
 * compressors that use them.  This isn't just about saving the overhead of a
 * function call.  These functions are intended to be called from the inner
 * loops of compressors, where giving the compiler more control over register
 * allocation is very helpful.  There is also significant benefit to be gained
 * from allowing the CPU to predict branches independently at each call site.
 * For example, "lazy"-style compressors can be written with two calls to
 * longest_match(), each of which starts with a different 'best_len' and
 * therefore has significantly different performance characteristics.
 *
 * Although any hash function can be used, a multiplicative hash is fast and
 * works well.
 *
 * On some processors, it is significantly faster to extend matches by whole
 * words (32 or 64 bits) instead of by individual bytes.  For this to be the
 * case, the processor must implement unaligned memory accesses efficiently and
 * must have either a fast "find first set bit" instruction or a fast "find last
 * set bit" instruction, depending on the processor's endianness.
 *
 * The code uses one loop for finding the first match and one loop for finding a
 * longer match.  Each of these loops is tuned for its respective task and in
 * combination are faster than a single generalized loop that handles both
 * tasks.
 *
 * The code also uses a tight inner loop that only compares the last and first
 * bytes of a potential match.  It is only when these bytes match that a full
 * match extension is attempted.
 *
 * Optionally, each linked list node can store not just the next node but the
 * next several nodes ("hops"); see 'next_tab'.  This makes walking the lists
 * much faster on modern CPUs, at the cost of more memory and slightly slower
 * insertion.  It's worthwhile for compressors that search deeply.
 *
 * ----------------------------------------------------------------------------
 */

#ifndef LIB_HC_MATCHFINDER_H
#define LIB_HC_MATCHFINDER_H

#include "matchfinder_common.h"

#define HC_MATCHFINDER_HASH3_ORDER	15
#define HC_MATCHFINDER_HASH4_ORDER	16

#define HC_MATCHFINDER_TOTAL_HASH_SIZE			\
	(((1UL << HC_MATCHFINDER_HASH3_ORDER) +		\
	  (1UL << HC_MATCHFINDER_HASH4_ORDER)) * sizeof(mf_pos_t))

/*
 * Type of a multi-hop link in 'next_tab'.  This is 'machine_word_t', annotated
 * with 'may_alias' because matchfinder_rebase() accesses it as 'mf_pos_t'.
 */
#if defined(__GNUC__) || __has_attribute(may_alias)
typedef machine_word_t __attribute__((may_alias)) hc_link_t;
#else
typedef machine_word_t hc_link_t;
#endif

/* The number of hops in a multi-hop link; see 'next_tab'.  */
#define HC_MATCHFINDER_HOPS_PER_WORD	\
	((int)(sizeof(hc_link_t) / sizeof(mf_pos_t)))

struct MATCHFINDER_ALIGNED hc_matchfinder  {

	/* The hash table for finding length 3 matches  */
	mf_pos_t hash3_tab[1UL << HC_MATCHFINDER_HASH3_ORDER];

	/* The hash table which contains the first nodes of the linked lists for
	 * finding length 4+ matches  */
	mf_pos_t hash4_tab[1UL << HC_MATCHFINDER_HASH4_ORDER];

	/*
	 * The "links" from each linked list node to the nodes that follow it.
	 * The link of the node for the sequence with position 'pos' is either
	 * 'single_hop[pos]' or 'multi_hop[pos]', depending on the 'multi_hop'
	 * parameter of the matchfinder functions.  'multi_hop' must be a
	 * compile-time constant, and it must be the same for all calls that use
	 * the same matchfinder.
	 *
	 * A single-hop link is just the next node.  A multi-hop link is the
	 * next HC_MATCHFINDER_HOPS_PER_WORD nodes, packed as 16-bit fields: the
	 * next node in bits 0-15, the node after that in bits 16-31, and so on.
	 *
	 * Multi-hop links make the linked lists faster to walk.  Walking a list
	 * is otherwise a chain of dependent memory loads, where each load's
	 * address comes from the previous load; and since the links don't fit
	 * in the L1 cache, each step can take the full latency of an L2 cache
	 * access.  With N hops per link, only one in every N steps needs to
	 * wait for a load.
	 *
	 * The extra hops are easy to maintain, as the lists are only ever
	 * modified by prepending a node: the new node's link is just the old
	 * first node, followed by the first N-1 hops of the old first node's
	 * link.  The visited nodes are exactly the same as with single-hop
	 * links, so the matches found are too.  However, inserting a node now
	 * requires loading the old first node's link, which makes insertion
	 * slower.  So multi-hop links are worthwhile only when searching
	 * deeply.
	 *
	 * Each hop is a 16-bit field, so either kind of link can be rebased
	 * together with the hash tables as an array of mf_pos_t.
	 *
	 * This must be the last member, so that the space for multi-hop links
	 * needn't be allocated when they aren't used; see
	 * hc_matchfinder_size().
	 */
	union {
		mf_pos_t single_hop[MATCHFINDER_WINDOW_SIZE];
		hc_link_t multi_hop[MATCHFINDER_WINDOW_SIZE];
	} next_tab;
};

/*
 * Return the number of bytes at the start of struct hc_matchfinder that are
 * used.  When multi-hop links aren't used, the rest needn't be allocated.
 */
static forceinline size_t
hc_matchfinder_size(bool multi_hop)
{
	return offsetof(struct hc_matchfinder, next_tab) +
	       MATCHFINDER_WINDOW_SIZE *
	       (multi_hop ? sizeof(hc_link_t) : sizeof(mf_pos_t));
}

/* Return the multi-hop link of the node with position @node.  */
static forceinline hc_link_t
hc_matchfinder_get_link(const struct hc_matchfinder *mf, mf_pos_t node)
{
	return mf->next_tab.multi_hop[node & (MATCHFINDER_WINDOW_SIZE - 1)];
}

/* Return the last node of the multi-hop link @link.  */
static forceinline mf_pos_t
hc_last_hop(hc_link_t link)
{
	return (mf_pos_t)(link >> (16 * (HC_MATCHFINDER_HOPS_PER_WORD - 1)));
}

/*
 * Prepend the node for the sequence with position @cur_pos to the linked list
 * whose first node is @old_first.  If @multi_hop, return the link of @old_first
 * (which is needed to build the new node's link anyway); otherwise return 0.
 */
static forceinline hc_link_t
hc_matchfinder_prepend(struct hc_matchfinder *mf, u32 cur_pos,
		       mf_pos_t old_first, bool multi_hop)
{
	hc_link_t old_first_link;

	if (!multi_hop) {
		mf->next_tab.single_hop[cur_pos] = old_first;
		return 0;
	}
	old_first_link = hc_matchfinder_get_link(mf, old_first);
	mf->next_tab.multi_hop[cur_pos] =
		(old_first_link << 16) | (u16)old_first;
	return old_first_link;
}

/* Prepare the matchfinder for a new input buffer.  */
static forceinline void
hc_matchfinder_init(struct hc_matchfinder *mf, bool multi_hop)
{
	STATIC_ASSERT(HC_MATCHFINDER_TOTAL_HASH_SIZE %
		      MATCHFINDER_SIZE_ALIGNMENT == 0);

	matchfinder_init((mf_pos_t *)mf, HC_MATCHFINDER_TOTAL_HASH_SIZE);

	/*
	 * With multi-hop links, the first node inserted will have the link of
	 * MATCHFINDER_INITVAL, i.e. of position 0, copied into its link.  The
	 * copied hops will never be visited, but make sure they're initialized
	 * anyway.  The rest of 'next_tab' needn't be initialized, since every
	 * other link that gets read is a link that was written.
	 */
	if (multi_hop)
		mf->next_tab.multi_hop[0] = (hc_link_t)0x0001000100010001 *
					    (u16)MATCHFINDER_INITVAL;
}

static forceinline void
hc_matchfinder_slide_window(struct hc_matchfinder *mf, bool multi_hop)
{
	STATIC_ASSERT(sizeof(mf->next_tab.single_hop) %
		      MATCHFINDER_SIZE_ALIGNMENT == 0);
	STATIC_ASSERT(sizeof(*mf) % MATCHFINDER_SIZE_ALIGNMENT == 0);

	/* Rebase the hash tables and the part of 'next_tab' that is used.  */
	matchfinder_rebase((mf_pos_t *)mf, hc_matchfinder_size(multi_hop));
}

/*
 * The longest input that is compressed as a small input; see "Small inputs" in
 * matchfinder_common.h.  A slot of the two maps is a bucket and the hash value.
 */
#define HC_MATCHFINDER_MAX_SMALL_INPUT_LENGTH	384
#define HC_MATCHFINDER_SLOT_SIZE		2

/*
 * Prepare the matchfinder for a small input: two maps of 1 << @order slots.
 * 'next_tab' is indexed by position and is used as for any input.
 */
static forceinline void
hc_matchfinder_init_small(struct hc_matchfinder *mf, bool multi_hop,
			  unsigned order)
{
	STATIC_ASSERT(((HC_MATCHFINDER_SLOT_SIZE * sizeof(mf_pos_t))
		       << MATCHFINDER_MIN_SMALL_ORDER) %
		      MATCHFINDER_SIZE_ALIGNMENT == 0);
	STATIC_ASSERT(sizeof(mf->hash3_tab) % MATCHFINDER_MEM_ALIGNMENT == 0);
	/* See matchfinder_small_order() for the largest maps. */
	STATIC_ASSERT((2 << MATCHFINDER_SMALL_SPARE_ORDER) *
		      HC_MATCHFINDER_MAX_SMALL_INPUT_LENGTH <=
		      MIN(sizeof(mf->hash3_tab) / (HC_MATCHFINDER_SLOT_SIZE *
						   sizeof(mf_pos_t)),
			  1UL << HC_MATCHFINDER_HASH3_ORDER));

	matchfinder_init(mf->hash3_tab,
			 (HC_MATCHFINDER_SLOT_SIZE * sizeof(mf_pos_t)) << order);
	matchfinder_init(mf->hash4_tab,
			 (HC_MATCHFINDER_SLOT_SIZE * sizeof(mf_pos_t)) << order);

	/* See hc_matchfinder_init(). */
	if (multi_hop)
		mf->next_tab.multi_hop[0] = (hc_link_t)0x0001000100010001 *
					    (u16)MATCHFINDER_INITVAL;
}

/*
 * hc_matchfinder_longest_match() and hc_matchfinder_skip_bytes() are in
 * hc_matchfinder_template.h, which is compiled twice: for inputs in general,
 * and with the suffix "_small" for small inputs.
 */
#define SUFFIX
#define SMALL_INPUT	0
#include "hc_matchfinder_template.h"

#define SUFFIX		_small
#define SMALL_INPUT	1
#include "hc_matchfinder_template.h"

#endif /* LIB_HC_MATCHFINDER_H */
