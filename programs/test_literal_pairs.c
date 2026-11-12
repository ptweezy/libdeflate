/*
 * test_literal_pairs.c
 *
 * Test decompression of data for which the decompressor makes pairs of literals
 * in the litlen decode table, i.e. data whose literals have short codewords.
 *
 * This checks that such data is decompressed correctly, and that the buffer
 * bounds are respected, at many different sizes and with output buffers that
 * are exact, too large, and too small, as well as with truncated input.  The
 * different sizes exercise the transitions between the decompressor's fastloop
 * and its generic loop at different points relative to the pairs.
 */

#include "test_util.h"

static u32 rng_state;

static u32
rand32(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/*
 * Map a small integer to a byte value.  The values are spread out so that
 * literals with the high bit set get used both first and second in pairs.
 */
static u8
spread(u32 i)
{
	return (i * 73 + 190) & 0xFF;
}

static void
generate_data(u8 *data, size_t size, unsigned pattern)
{
	size_t i = 0;

	rng_state = 0x9E3779B9 + pattern;
	while (i < size) {
		u32 r = rand32();

		switch (pattern) {
		case 0: /* A single literal value, without long runs */
			data[i++] = (r % 61 == 0) ? 0xFF : 0x80;
			break;
		case 1: /* 2 equiprobable values */
			data[i++] = spread(r & 1);
			break;
		case 2: /* 3 equiprobable values */
			data[i++] = spread(r % 3);
			break;
		case 3: /* 7 equiprobable values */
			data[i++] = spread(r % 7);
			break;
		case 4: /* 40 equiprobable values */
			data[i++] = spread(r % 40);
			break;
		case 5: { /* Geometric distribution around 0, as bytes */
			int v = 0;

			while ((rand32() & 3) != 0 && v < 60)
				v++;
			data[i++] = (r & 1) ? v : -v;
			break;
		}
		case 6: /* Skewed, with all 256 values used (long codewords) */
			if (r % 16 != 0)
				data[i++] = spread((r >> 8) % 5);
			else
				data[i++] = r >> 24;
			break;
		default: /* Skewed literals mixed with matches of all lengths */
			if (r % 8 != 0 || i < 300) {
				data[i++] = spread((r >> 8) % 6);
			} else {
				size_t len = 3 + ((r >> 8) % 256);
				size_t offset = 1 + ((r >> 16) % 299);

				for (; len != 0 && i < size; len--, i++)
					data[i] = data[i - offset];
			}
			break;
		}
	}
}

static void
do_decompress(struct libdeflate_decompressor *d,
	      const u8 *in, size_t in_size, const u8 *expected, size_t size,
	      size_t out_avail, bool use_actual_out_nbytes)
{
	u8 *in_start, *in_end, *out_start, *out_end;
	size_t actual_out_nbytes = SIZE_MAX;
	enum libdeflate_result res;

	alloc_guarded_buffer(in_size, &in_start, &in_end);
	alloc_guarded_buffer(out_avail, &out_start, &out_end);
	memcpy(in_start, in, in_size);

	res = libdeflate_deflate_decompress(d, in_start, in_size,
					    out_start, out_avail,
					    use_actual_out_nbytes ?
					    &actual_out_nbytes : NULL);
	if (expected == NULL) {
		/* Truncated input */
		ASSERT(res != LIBDEFLATE_SUCCESS);
	} else if (out_avail < size) {
		ASSERT(res == LIBDEFLATE_INSUFFICIENT_SPACE);
	} else if (out_avail > size && !use_actual_out_nbytes) {
		ASSERT(res == LIBDEFLATE_SHORT_OUTPUT);
	} else {
		ASSERT(res == LIBDEFLATE_SUCCESS);
		if (use_actual_out_nbytes)
			ASSERT(actual_out_nbytes == size);
		ASSERT(memcmp(out_start, expected, size) == 0);
	}
	free_guarded_buffer(in_start, in_end);
	free_guarded_buffer(out_start, out_end);
}

int
tmain(int argc, tchar *argv[])
{
	static const size_t sizes[] = {
		/* Around the point where pairs start being considered */
		1500, 2000, 2500, 3000, 4000,
		/* Various larger sizes, some with multiple blocks */
		5000, 5001, 5002, 5003, 9999, 20000, 70000, 150001,
	};
	static const int levels[] = { 1, 6, 9, 12 };
	const size_t max_size = sizes[ARRAY_LEN(sizes) - 1];
	struct libdeflate_decompressor *d;
	u8 *data, *compressed;
	size_t max_csize;
	unsigned pattern;
	size_t i, j, k;

	begin_program(argv);

	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);
	data = xmalloc(max_size);
	max_csize = max_size + 1000;
	compressed = xmalloc(max_csize);

	for (pattern = 0; pattern < 8; pattern++) {
		generate_data(data, max_size, pattern);
		for (i = 0; i < ARRAY_LEN(sizes); i++) {
			const size_t size = sizes[i];

			for (j = 0; j < ARRAY_LEN(levels); j++) {
				struct libdeflate_compressor *c;
				size_t csize;

				c = libdeflate_alloc_compressor(levels[j]);
				ASSERT(c != NULL);
				csize = libdeflate_deflate_compress(
						c, data, size,
						compressed, max_csize);
				ASSERT(csize != 0);
				libdeflate_free_compressor(c);

				/* Exact output size */
				do_decompress(d, compressed, csize, data, size,
					      size, false);
				do_decompress(d, compressed, csize, data, size,
					      size, true);
				/* Output buffer too large */
				do_decompress(d, compressed, csize, data, size,
					      size + 1, true);
				do_decompress(d, compressed, csize, data, size,
					      size + 400, true);
				do_decompress(d, compressed, csize, data, size,
					      size + 3, false);
				/* Output buffer too small */
				for (k = 1; k <= 5; k++)
					do_decompress(d, compressed, csize,
						      data, size, size - k,
						      k & 1);
				do_decompress(d, compressed, csize, data, size,
					      size - 301, true);
				do_decompress(d, compressed, csize, data, size,
					      size / 2, false);
				/* Truncated input */
				for (k = 1; k <= 3; k++)
					do_decompress(d, compressed, csize - k,
						      NULL, size, size, k & 1);
				do_decompress(d, compressed, csize / 2,
					      NULL, size, size, true);
			}
		}
	}

	free(data);
	free(compressed);
	libdeflate_free_decompressor(d);
	return 0;
}
