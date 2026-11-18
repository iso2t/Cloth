// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <string_view>

namespace cloth {

// Returns the byte length of the first UTF-8 scalar, or zero for invalid input.
inline std::size_t utf8_scalar_length (std::string_view bytes) {
	if (bytes.empty()) {
		return 0;
	}

	const auto first = static_cast<unsigned char>(bytes.front());
	if (first < 0x80) {
		return 1;
	}

	// Bound the second byte to reject overlong encodings, surrogates and
	// code points beyond U+10FFFF.
	std::size_t   length;
	unsigned char secondMin = 0x80;
	unsigned char secondMax = 0xBF;
	if (first >= 0xC2 && first <= 0xDF) {
		length = 2;
	} else if (first >= 0xE0 && first <= 0xEF) {
		length = 3;
		if (first == 0xE0) {
			secondMin = 0xA0;
		} else if (first == 0xED) {
			secondMax = 0x9F;
		}
	} else if (first >= 0xF0 && first <= 0xF4) {
		length = 4;
		if (first == 0xF0) {
			secondMin = 0x90;
		} else if (first == 0xF4) {
			secondMax = 0x8F;
		}
	} else {
		return 0;
	}

	if (bytes.size() < length) {
		return 0;
	}

	const auto second = static_cast<unsigned char>(bytes[1]);
	if (second < secondMin || second > secondMax) {
		return 0;
	}

	for (std::size_t i = 2; i < length; ++i) {
		const auto next = static_cast<unsigned char>(bytes[i]);
		if (next < 0x80 || next > 0xBF) {
			return 0;
		}
	}
	return length;
}

// Decodes one scalar after utf8_scalar_length has validated its bytes.
inline char32_t utf8_scalar_value (std::string_view scalar) {
	const auto c_first = static_cast<unsigned char>(scalar.front());
	if (scalar.size() == 1) {
		return c_first;
	}
	char32_t c32_value = c_first & (0x7Fu >> scalar.size());
	for (std::size_t st_index = 1; st_index < scalar.size(); ++st_index) {
		c32_value = (c32_value << 6) | (static_cast<unsigned char>(scalar[st_index]) & 0x3Fu);
	}
	return c32_value;
}
}
