/*
 * matchfinder_common.h - common code for Lempel-Ziv matchfinding
 */

#ifndef LIB_MATCHFINDER_COMMON_H
#define LIB_MATCHFINDER_COMMON_H

#include "lib_common.h"

#ifndef MATCHFINDER_WINDOW_ORDER
#  error "MATCHFINDER_WINDOW_ORDER must be defined!"
#endif

/*
 * Given a 32-bit value that was loaded with the platform's native endianness,
 * return a 32-bit value whose high-order 8 bits are 0 and whose low-order 24
 * bits contain the first 3 bytes, arranged in octets in a platform-dependent
 * order, at the memory location from which the input 32-bit value was loaded.
 */
static forceinline u32
loaded_u32_to_u24(u32 v)
{
	if (CPU_IS_LITTLE_ENDIAN())
		return v & 0xFFFFFF;
	else
		return v >> 8;
}

/*
 * Load the next 3 bytes from @p into the 24 low-order bits of a 32-bit value.
 * The order in which the 3 bytes will be arranged as octets in the 24 bits is
 * platform-dependent.  At least 4 bytes (not 3) must be available at @p.
 */
static forceinline u32
load_u24_unaligned(const u8 *p)
{
#if UNALIGNED_ACCESS_IS_FAST
	return loaded_u32_to_u24(load_u32_unaligned(p));
#else
	if (CPU_IS_LITTLE_ENDIAN())
		return ((u32)p[0] << 0) | ((u32)p[1] << 8) | ((u32)p[2] << 16);
	else
		return ((u32)p[2] << 0) | ((u32)p[1] << 8) | ((u32)p[0] << 16);
#endif
}

#define MATCHFINDER_WINDOW_SIZE (1UL << MATCHFINDER_WINDOW_ORDER)

typedef s16 mf_pos_t;

#define MATCHFINDER_INITVAL ((mf_pos_t)-MATCHFINDER_WINDOW_SIZE)

/*
 * This is the memory address alignment, in bytes, required for the matchfinder
 * buffers by the architecture-specific implementations of matchfinder_init()
 * and matchfinder_rebase().  "Matchfinder buffer" means an entire struct
 * hc_matchfinder, bt_matchfinder, or ht_matchfinder; the next_tab field of
 * struct hc_matchfinder; or the child_tab field of struct bt_matchfinder.
 *
 * This affects how the entire 'struct deflate_compressor' is allocated, since
 * the matchfinder structures are embedded inside it.
 *
 * Currently the maximum memory address alignment required is 32 bytes, needed
 * by the AVX-2 matchfinder functions.
 */
#define MATCHFINDER_MEM_ALIGNMENT	32

/*
 * This declares a size, in bytes, that is guaranteed to divide the sizes of the
 * matchfinder buffers (where "matchfinder buffers" is as defined for
 * MATCHFINDER_MEM_ALIGNMENT).  The architecture-specific implementations of
 * matchfinder_init() and matchfinder_rebase() take advantage of this value.
 *
 * Currently the maximum size alignment required is 128 bytes, needed by
 * the AVX-2 matchfinder functions.  However, the RISC-V Vector Extension
 * matchfinder functions can, in principle, take advantage of a larger size
 * alignment.  Therefore, we set this to 1024, which still easily divides the
 * actual sizes that result from the current matchfinder struct definitions.
 * This value can safely be changed to any power of two that is >= 128.
 */
#define MATCHFINDER_SIZE_ALIGNMENT	1024

#undef matchfinder_init
#undef matchfinder_rebase
#ifdef _aligned_attribute
#  define MATCHFINDER_ALIGNED _aligned_attribute(MATCHFINDER_MEM_ALIGNMENT)
#  if defined(ARCH_ARM32) || defined(ARCH_ARM64)
#    include "arm/matchfinder_impl.h"
#  elif defined(ARCH_RISCV)
#    include "riscv/matchfinder_impl.h"
#  elif defined(ARCH_X86_32) || defined(ARCH_X86_64)
#    include "x86/matchfinder_impl.h"
#  endif
#else
#  define MATCHFINDER_ALIGNED
#endif

/*
 * Initialize the hash table portion of the matchfinder.
 *
 * Essentially, this is an optimized memset().
 *
 * 'data' must be aligned to a MATCHFINDER_MEM_ALIGNMENT boundary, and
 * 'size' must be a multiple of MATCHFINDER_SIZE_ALIGNMENT.
 */
#ifndef matchfinder_init
static forceinline void
matchfinder_init(mf_pos_t *data, size_t size)
{
	size_t num_entries = size / sizeof(*data);
	size_t i;

	for (i = 0; i < num_entries; i++)
		data[i] = MATCHFINDER_INITVAL;
}
#endif

/*
 * Slide the matchfinder by MATCHFINDER_WINDOW_SIZE bytes.
 *
 * This must be called just after each MATCHFINDER_WINDOW_SIZE bytes have been
 * run through the matchfinder.
 *
 * This subtracts MATCHFINDER_WINDOW_SIZE bytes from each entry in the given
 * array, making the entries be relative to the current position rather than the
 * position MATCHFINDER_WINDOW_SIZE bytes prior.  To avoid integer underflows,
 * entries that would become less than -MATCHFINDER_WINDOW_SIZE stay at
 * -MATCHFINDER_WINDOW_SIZE, keeping them permanently out of bounds.
 *
 * The given array must contain all matchfinder data that is position-relative:
 * the hash table(s) as well as any hash chain or binary tree links.  Its
 * address must be aligned to a MATCHFINDER_MEM_ALIGNMENT boundary, and its size
 * must be a multiple of MATCHFINDER_SIZE_ALIGNMENT.
 */
#ifndef matchfinder_rebase
static forceinline void
matchfinder_rebase(mf_pos_t *data, size_t size)
{
	size_t num_entries = size / sizeof(*data);
	size_t i;

	if (MATCHFINDER_WINDOW_SIZE == 32768) {
		/*
		 * Branchless version for 32768-byte windows.  Clear all bits if
		 * the value was already negative, then set the sign bit.  This
		 * is equivalent to subtracting 32768 with signed saturation.
		 */
		for (i = 0; i < num_entries; i++)
			data[i] = 0x8000 | (data[i] & ~(data[i] >> 15));
	} else {
		for (i = 0; i < num_entries; i++) {
			if (data[i] >= 0)
				data[i] -= (mf_pos_t)-MATCHFINDER_WINDOW_SIZE;
			else
				data[i] = (mf_pos_t)-MATCHFINDER_WINDOW_SIZE;
		}
	}
}
#endif

/*
 * The hash function: given a sequence prefix held in the low-order bits of a
 * 32-bit value, multiply by a carefully-chosen large constant.  Discard any
 * bits of the product that don't fit in a 32-bit value, but take the
 * next-highest @num_bits bits of the product as the hash value, as those have
 * the most randomness.
 */
static forceinline u32
lz_hash(u32 seq, unsigned num_bits)
{
	return (u32)(seq * 0x1E35A7BD) >> (32 - num_bits);
}

/*
 * Return the number of bytes at @matchptr that match the bytes at @strptr, up
 * to a maximum of @max_len.  Initially, @start_len bytes are matched.
 */
static forceinline u32
lz_extend(const u8 * const strptr, const u8 * const matchptr,
	  const u32 start_len, const u32 max_len)
{
	u32 len = start_len;
	machine_word_t v_word;

	if (UNALIGNED_ACCESS_IS_FAST) {

		if (likely(max_len - len >= 4 * WORDBYTES)) {

		#define COMPARE_WORD_STEP				\
			v_word = load_word_unaligned(&matchptr[len]) ^	\
				 load_word_unaligned(&strptr[len]);	\
			if (v_word != 0)				\
				goto word_differs;			\
			len += WORDBYTES;				\

			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
		#undef COMPARE_WORD_STEP
		}

		while (len + WORDBYTES <= max_len) {
			v_word = load_word_unaligned(&matchptr[len]) ^
				 load_word_unaligned(&strptr[len]);
			if (v_word != 0)
				goto word_differs;
			len += WORDBYTES;
		}
	}

	while (len < max_len && matchptr[len] == strptr[len])
		len++;
	return len;

word_differs:
	if (CPU_IS_LITTLE_ENDIAN())
		len += (bsfw(v_word) >> 3);
	else
		len += (WORDBITS - 1 - bsrw(v_word)) >> 3;
	return len;
}

/*
 * Small inputs
 *
 * Initializing the hash tables of a matchfinder takes longer than compressing
 * a small input does, and a small input uses only a few of their buckets.  So
 * for a small input, the start of each hash table is used as an open-addressed
 * map from the hash value to the bucket, and only that part is initialized.
 *
 * A map is '1 << order' slots.  A slot is a bucket followed by the hash value
 * that the bucket belongs to.  A slot is free while the first entry of its
 * bucket is MATCHFINDER_INITVAL; the matchfinders store a position there right
 * after each lookup.  The bucket of a hash value in the map holds exactly what
 * its bucket in the full table would hold, so the same matches are found.
 *
 * The matchfinders' search functions are compiled twice from template headers:
 * for inputs in general, and for small inputs.
 */

/* log2 of the number of slots per input byte; at least 1 */
#define MATCHFINDER_SMALL_SPARE_ORDER	3

/* The smallest map order; see matchfinder_small_order() */
#define MATCHFINDER_MIN_SMALL_ORDER	8

/*
 * Return the order of the maps for a small input of @in_nbytes bytes
 * (@in_nbytes > 0).  '1 << order' is less than
 * '(2 << MATCHFINDER_SMALL_SPARE_ORDER) * in_nbytes' unless the minimum
 * applies.
 */
static forceinline unsigned
matchfinder_small_order(size_t in_nbytes)
{
	return MAX(bsr32(2 * in_nbytes - 1) + MATCHFINDER_SMALL_SPARE_ORDER,
		   MATCHFINDER_MIN_SMALL_ORDER);
}

/*
 * Return the bucket of @hash, a hash value of @hash_order bits, in the map
 * @slots, whose slots are @slot_size entries each: the slot that has this hash
 * value, or else the first free slot at or after the hash value's home slot,
 * which then belongs to this hash value.
 */
static forceinline mf_pos_t *
matchfinder_small_bucket(mf_pos_t *slots, unsigned slot_size, u32 hash,
			 unsigned hash_order, unsigned order)
{
	const u32 mask = ((u32)1 << order) - 1;
	u32 i = hash >> (hash_order - order);

	for (;;) {
		mf_pos_t *slot = &slots[slot_size * i];
		/* 0 if the slot belongs to this hash value */
		const u32 other = (u16)(slot[slot_size - 1] ^ hash);
		/* 0 if the slot is free */
		const u32 used = (u16)(slot[0] ^ MATCHFINDER_INITVAL);

		/*
		 * Whether the hash value has a slot already depends on the
		 * data, so test for "its slot" and "a free slot" with one
		 * branch; the caller tests the bucket anyway.  The test is
		 * written so that the compiler cannot split it into two
		 * branches, as gcc does with 'other == 0 | used == 0'.
		 */
		if (likely((s32)((other - 1) | (used - 1)) < 0)) {
			slot[slot_size - 1] = hash;
			return slot;
		}
		i = (i + 1) & mask;
	}
}

#endif /* LIB_MATCHFINDER_COMMON_H */
