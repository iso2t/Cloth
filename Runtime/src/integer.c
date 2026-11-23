// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static ptrdiff_t native_word_stride (void) {
	const uint32_t one = 1;
	return *(const unsigned char *)&one ? 1 : -1;
}

static ptrdiff_t significant_words (const uint32_t *value, ptrdiff_t words, ptrdiff_t stride) {
	while (words && value[(words - 1) * stride] == 0) --words;
	return words;
}

// Write the low word_count words into a separate output buffer. All buffers use the target's native integer layout.
void cloth_rt_mul_words (uint32_t *product, const uint32_t *left, const uint32_t *right, uint32_t word_count) {
	if (!word_count) return;
	const ptrdiff_t words  = word_count;
	const ptrdiff_t stride = native_word_stride();
	memset(product, 0, words * sizeof(*product));
	if (stride < 0) {
		product += words - 1;
		left += words - 1;
		right += words - 1;
	}
	const ptrdiff_t left_words  = significant_words(left, words, stride);
	const ptrdiff_t right_words = significant_words(right, words, stride);
	for (ptrdiff_t i = 0; i < left_words; ++i) {
		const uint32_t a = left[i * stride];
		if (!a) continue;
		const ptrdiff_t count = right_words < words - i ? right_words : words - i;
		uint64_t        carry = 0;
		for (ptrdiff_t j = 0; j < count; ++j) {
			const ptrdiff_t offset  = (i + j) * stride;
			const uint64_t  partial = (uint64_t)a * right[j * stride] + product[offset] + carry;
			product[offset]         = (uint32_t)partial;
			carry                   = partial >> 32;
		}
		if (i + count < words) product[(i + count) * stride] = (uint32_t)carry;
	}
}

static void negate_words (uint32_t *value, ptrdiff_t words, ptrdiff_t stride) {
	uint64_t carry = 1;
	for (ptrdiff_t i = 0; i < words; ++i) {
		const uint64_t negated = (uint64_t)~value[i * stride] + carry;
		value[i * stride]      = (uint32_t)negated;
		carry                  = negated >> 32;
	}
}

static bool at_least (const uint32_t *left, const uint32_t *right, ptrdiff_t words, ptrdiff_t stride) {
	for (ptrdiff_t i = words; i-- > 0;) {
		if (left[i * stride] != right[i * stride]) return left[i * stride] > right[i * stride];
	}
	return true;
}

static void subtract_words (uint32_t *left, const uint32_t *right, ptrdiff_t words, ptrdiff_t stride) {
	uint64_t borrow = 0;
	for (ptrdiff_t i = 0; i < words; ++i) {
		const uint32_t original   = left[i * stride];
		const uint64_t subtrahend = (uint64_t)right[i * stride] + borrow;
		left[i * stride]          = (uint32_t)(original - subtrahend);
		borrow                    = original < subtrahend;
	}
}

static void divide_magnitudes (uint32_t *dividend, const uint32_t *divisor, uint32_t *remainder, ptrdiff_t words, ptrdiff_t stride) {
	const ptrdiff_t dividend_words = significant_words(dividend, words, stride);
	const ptrdiff_t divisor_words  = significant_words(divisor, words, stride);
	if (divisor_words == 1) {
		uint64_t carry = 0;
		for (ptrdiff_t i = dividend_words; i-- > 0;) {
			const uint64_t partial = (carry << 32) | dividend[i * stride];
			dividend[i * stride]   = (uint32_t)(partial / divisor[0]);
			carry                  = partial % divisor[0];
		}
		remainder[0] = (uint32_t)carry;
		return;
	}

	// A prefix shorter than the divisor already fits in the remainder.
	const ptrdiff_t pending = dividend_words >= divisor_words ? dividend_words - divisor_words + 1 : 0;
	for (ptrdiff_t i = pending; i < dividend_words; ++i) {
		remainder[(i - pending) * stride] = dividend[i * stride];
		dividend[i * stride]              = 0;
	}
	for (ptrdiff_t i = pending; i-- > 0;) {
		const uint32_t input    = dividend[i * stride];
		uint32_t       quotient = 0;
		for (unsigned bit = 32; bit-- > 0;) {
			uint32_t carry = (input >> bit) & 1u;
			for (ptrdiff_t j = 0; j < divisor_words; ++j) {
				const uint32_t previous = remainder[j * stride];
				remainder[j * stride]   = (previous << 1) | carry;
				carry                   = previous >> 31;
			}
			// The shifted remainder is less than twice the divisor, so one subtraction suffices.
			if (carry || at_least(remainder, divisor, divisor_words, stride)) {
				subtract_words(remainder, divisor, divisor_words, stride);
				quotient |= UINT32_C(1) << bit;
			}
		}
		dividend[i * stride] = quotient;
	}
}

// The caller checks for zero and signed overflow at the Cloth width, then extends into three disjoint native-order buffers.
// The quotient replaces the dividend. The divisor is scratch, and the remainder takes the dividend's sign.
void cloth_rt_divmod_words (uint32_t *dividend, uint32_t *divisor, uint32_t *remainder, uint32_t word_count, uint32_t signed_operands) {
	if (!word_count) return;
	const ptrdiff_t words  = word_count;
	const ptrdiff_t stride = native_word_stride();
	memset(remainder, 0, words * sizeof(*remainder));
	if (stride < 0) {
		dividend += words - 1;
		divisor += words - 1;
		remainder += words - 1;
	}
	const ptrdiff_t high              = (words - 1) * stride;
	const bool      negative_dividend = signed_operands && dividend[high] >> 31;
	const bool      negative_divisor  = signed_operands && divisor[high] >> 31;
	if (negative_dividend) negate_words(dividend, words, stride);
	if (negative_divisor) negate_words(divisor, words, stride);
	divide_magnitudes(dividend, divisor, remainder, words, stride);
	if (negative_dividend != negative_divisor) negate_words(dividend, words, stride);
	if (negative_dividend) negate_words(remainder, words, stride);
}
