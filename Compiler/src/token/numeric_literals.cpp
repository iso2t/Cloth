// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utf8proc/utf8proc.h>

#include <diagnostics.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>
#include <util/utf8.hpp>

namespace cloth {

namespace {

constexpr int WORD_BITS = std::numeric_limits<std::uint32_t>::digits;
// Seven hex digits keep the chunk radix within one word.
constexpr std::size_t DIGITS_PER_CHUNK = 7;

std::string normalizedNumber (std::string_view text, const bool decimal) {
	std::string s_result;
	s_result.reserve(text.size());
	while (!text.empty()) {
		const char c_first = text.front();
		if (decimal && static_cast<unsigned char>(c_first) >= 0x80) {
			if (const auto [ascii, length] = decimalDigit(text); length != 0) {
				s_result.push_back(ascii);
				text.remove_prefix(length);
				continue;
			}
		}
		if (c_first != '_') {
			s_result.push_back(c_first);
		}
		text.remove_prefix(1);
	}
	return s_result;
}

}

DecimalDigit decimalDigit (const std::string_view text) {
	if (text.empty()) {
		return {};
	}
	if (const auto c_first = static_cast<unsigned char>(text.front()); c_first < 0x80) {
		return c_first >= '0' && c_first <= '9' ? DecimalDigit{ .ascii = static_cast<char>(c_first), .length = 1 } : DecimalDigit{};
	}
	const std::size_t st_length = utf8ScalarLength(text);
	if (st_length == 0) return {};
	const auto scalar = static_cast<utf8proc_int32_t>(utf8ScalarValue(text.substr(0, st_length)));
	if (utf8proc_category(scalar) != UTF8PROC_CATEGORY_ND) return {};

	// Unicode decimal digits form ordered sets of ten. Several sets can be adjacent.
	utf8proc_int32_t runStart = scalar;
	while (utf8proc_category(runStart - 1) == UTF8PROC_CATEGORY_ND) --runStart;
	return { .ascii = static_cast<char>('0' + (scalar - runStart) % 10), .length = st_length };
}

int integerLiteralRadix (const std::string_view spelling) {
	if (spelling.starts_with("0x")) {
		return 16;
	}
	if (spelling.starts_with("0b")) {
		return 2;
	}
	if (spelling.starts_with("0o")) {
		return 8;
	}
	return 10;
}

IntegerValue integerLiteralValue (const Token &token) {
	if (token.getType() != TokenType::IntegerLiteral) {
		throw cde::SyntaxErrors::number(token, /*floating=*/false);
	}
	std::string_view s_spelling = token.getLexeme();
	const int        i_radix    = integerLiteralRadix(s_spelling);
	if (i_radix != 10) {
		s_spelling.remove_prefix(2);
	} else if (s_spelling.ends_with('i') || s_spelling.ends_with('I')) {
		s_spelling.remove_suffix(1);
	}
	const std::string s_digits = normalizedNumber(s_spelling, i_radix == 10);
	if (s_digits.empty()) {
		throw cde::SyntaxErrors::number(token, /*floating=*/false);
	}

	IntegerValue     v_words{ 0 };
	std::string_view s_remaining = s_digits;
	while (!s_remaining.empty()) {
		const std::size_t st_length = std::min(s_remaining.size(), DIGITS_PER_CHUNK);
		const char       *c_end     = s_remaining.data() + st_length;
		std::uint32_t     u32_chunk = 0;
		if (const auto [ptr, ec] = std::from_chars(s_remaining.data(), c_end, u32_chunk, i_radix); ec != std::errc{} || ptr != c_end) {
			throw cde::SyntaxErrors::number(token, /*floating=*/false);
		}
		std::uint32_t u32_scale = 1;
		for (std::size_t st_digit = 0; st_digit < st_length; ++st_digit) {
			u32_scale *= static_cast<std::uint32_t>(i_radix);
		}

		// Multiply the accumulated magnitude by the chunk's radix, then add the chunk.
		std::uint64_t u64_carry = u32_chunk;
		for (std::uint32_t &u32_word : v_words) {
			u64_carry += static_cast<std::uint64_t>(u32_word) * u32_scale;
			u32_word = static_cast<std::uint32_t>(u64_carry);
			u64_carry >>= WORD_BITS;
		}
		if (u64_carry != 0) {
			if (v_words.size() == MAX_INTEGER_BITS / WORD_BITS) {
				throw cde::SyntaxErrors::number(token, /*floating=*/false);
			}
			v_words.push_back(static_cast<std::uint32_t>(u64_carry));
		}
		s_remaining.remove_prefix(st_length);
	}
	return v_words;
}

std::string floatingLiteralText (const Token &token) {
	if (token.getType() != TokenType::FloatingPointLiteral) {
		throw cde::SyntaxErrors::number(token, /*floating=*/true);
	}
	std::string_view s_spelling = token.getLexeme();
	if (s_spelling.ends_with('f') || s_spelling.ends_with('F')) {
		s_spelling.remove_suffix(1);
	}
	std::string s_text     = normalizedNumber(s_spelling, /*decimal=*/true);
	bool        b_hasPoint = false;
	bool        b_hasDigit = false;
	for (const char c_character : s_text) {
		if (c_character >= '0' && c_character <= '9') {
			b_hasDigit = true;
		} else if (c_character == '.' && !b_hasPoint) {
			b_hasPoint = true;
		} else {
			throw cde::SyntaxErrors::number(token, /*floating=*/true);
		}
	}
	if (!b_hasDigit) {
		throw cde::SyntaxErrors::number(token, /*floating=*/true);
	}
	if (!b_hasPoint) {
		s_text += ".0";
	} else if (s_text.back() == '.') {
		s_text.push_back('0');
	}
	return s_text;
}

}
