// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static ptrdiff_t nativeWordStride (void) {
	const uint32_t one = 1;
	return *(const unsigned char *)&one ? 1 : -1;
}

size_t clothRtIntegerZeros (const unsigned char *bits, const uint32_t width, const uint8_t leading) {
	const size_t bytes        = ((size_t)width + 7) / 8;
	const int    littleEndian = nativeWordStride() == 1;
	size_t       count        = 0;
	for (size_t n = 0; n < bytes; ++n) {
		const size_t   limb = leading ? bytes - 1 - n : n;
		const unsigned used = limb == bytes - 1 && width % 8 ? width % 8 : 8;
		const unsigned byte = bits[littleEndian ? limb : bytes - 1 - limb] & (1u << used) - 1;
		if (!byte) {
			count += used;
			continue;
		}
		unsigned mask = leading ? 1u << (used - 1) : 1u;
		while (!(byte & mask)) {
			++count;
			mask = leading ? mask >> 1 : mask << 1;
		}
		break;
	}
	return count;
}

static ptrdiff_t significantWords (const uint32_t *value, ptrdiff_t words, const ptrdiff_t stride) {
	while (words && value[(words - 1) * stride] == 0) --words;
	return words;
}

// Write the low wordCount words into a separate output buffer. All buffers use the target's native integer layout.
void clothRtMulWords (uint32_t *product, const uint32_t *left, const uint32_t *right, const uint32_t wordCount) {
	if (!wordCount) return;
	const ptrdiff_t words  = wordCount;
	const ptrdiff_t stride = nativeWordStride();
	memset(product, 0, words * sizeof(*product));
	if (stride < 0) {
		product += words - 1;
		left += words - 1;
		right += words - 1;
	}
	const ptrdiff_t leftWords  = significantWords(left, words, stride);
	const ptrdiff_t rightWords = significantWords(right, words, stride);
	for (ptrdiff_t i = 0; i < leftWords; ++i) {
		const uint32_t a = left[i * stride];
		if (!a) continue;
		const ptrdiff_t count = rightWords < words - i ? rightWords : words - i;
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

static void negateWords (uint32_t *value, const ptrdiff_t words, const ptrdiff_t stride) {
	uint64_t carry = 1;
	for (ptrdiff_t i = 0; i < words; ++i) {
		const uint64_t negated = (uint64_t)~value[i * stride] + carry;
		value[i * stride]      = (uint32_t)negated;
		carry                  = negated >> 32;
	}
}

static bool atLeast (const uint32_t *left, const uint32_t *right, const ptrdiff_t words, const ptrdiff_t stride) {
	for (ptrdiff_t i = words; i-- > 0;) {
		if (left[i * stride] != right[i * stride]) return left[i * stride] > right[i * stride];
	}
	return true;
}

static void subtractWords (uint32_t *left, const uint32_t *right, const ptrdiff_t words, const ptrdiff_t stride) {
	uint64_t borrow = 0;
	for (ptrdiff_t i = 0; i < words; ++i) {
		const uint32_t original   = left[i * stride];
		const uint64_t subtrahend = (uint64_t)right[i * stride] + borrow;
		left[i * stride]          = (uint32_t)(original - subtrahend);
		borrow                    = original < subtrahend;
	}
}

static void divideMagnitudes (uint32_t *dividend, const uint32_t *divisor, uint32_t *remainder, const ptrdiff_t words, const ptrdiff_t stride) {
	const ptrdiff_t dividendWords = significantWords(dividend, words, stride);
	const ptrdiff_t divisorWords  = significantWords(divisor, words, stride);
	if (divisorWords == 1) {
		uint64_t carry = 0;
		for (ptrdiff_t i = dividendWords; i-- > 0;) {
			const uint64_t partial = (carry << 32) | dividend[i * stride];
			dividend[i * stride]   = (uint32_t)(partial / divisor[0]);
			carry                  = partial % divisor[0];
		}
		remainder[0] = (uint32_t)carry;
		return;
	}

	// A prefix shorter than the divisor already fits in the remainder.
	const ptrdiff_t pending = dividendWords >= divisorWords ? dividendWords - divisorWords + 1 : 0;
	for (ptrdiff_t i = pending; i < dividendWords; ++i) {
		remainder[(i - pending) * stride] = dividend[i * stride];
		dividend[i * stride]              = 0;
	}
	for (ptrdiff_t i = pending; i-- > 0;) {
		const uint32_t input    = dividend[i * stride];
		uint32_t       quotient = 0;
		for (unsigned bit = 32; bit-- > 0;) {
			uint32_t carry = (input >> bit) & 1u;
			for (ptrdiff_t j = 0; j < divisorWords; ++j) {
				const uint32_t previous = remainder[j * stride];
				remainder[j * stride]   = (previous << 1) | carry;
				carry                   = previous >> 31;
			}
			// The shifted remainder is less than twice the divisor, so one subtraction suffices.
			if (carry || atLeast(remainder, divisor, divisorWords, stride)) {
				subtractWords(remainder, divisor, divisorWords, stride);
				quotient |= UINT32_C(1) << bit;
			}
		}
		dividend[i * stride] = quotient;
	}
}

// The caller checks for zero and signed overflow at the Cloth width, then extends into three disjoint native-order buffers.
// The quotient replaces the dividend. The divisor is scratch, and the remainder takes the dividend's sign.
void clothRtDivmodWords (uint32_t *dividend, uint32_t *divisor, uint32_t *remainder, const uint32_t wordCount, const uint32_t signedOperands) {
	if (!wordCount) return;
	const ptrdiff_t words  = wordCount;
	const ptrdiff_t stride = nativeWordStride();
	memset(remainder, 0, words * sizeof(*remainder));
	if (stride < 0) {
		dividend += words - 1;
		divisor += words - 1;
		remainder += words - 1;
	}
	const ptrdiff_t high             = (words - 1) * stride;
	const bool      negativeDividend = signedOperands && dividend[high] >> 31;
	const bool      negativeDivisor  = signedOperands && divisor[high] >> 31;
	if (negativeDividend) negateWords(dividend, words, stride);
	if (negativeDivisor) negateWords(divisor, words, stride);
	divideMagnitudes(dividend, divisor, remainder, words, stride);
	if (negativeDividend != negativeDivisor) negateWords(dividend, words, stride);
	if (negativeDividend) negateWords(remainder, words, stride);
}
