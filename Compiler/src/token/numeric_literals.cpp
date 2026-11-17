// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <syntax_errors.hpp>
#include <token/numeric_literals.hpp>
#include <util/utf8.hpp>

namespace cloth {

namespace {

// Decimal-digit ranges in the BMP, matching the Kotlin lexer's Char-based scanning.
constexpr char32_t DECIMAL_ZEROES[] = {
	0x0660, 0x06F0, 0x07C0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66, 0x0BE6, 0x0C66, 0x0CE6, 0x0D66, 0x0DE6, 0x0E50, 0x0ED0, 0x0F20, 0x1040, 0x1090,
	0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620, 0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10,
};

constexpr std::size_t MAX_INTEGER_BITS = 65536;
constexpr int         WORD_BITS        = std::numeric_limits<std::uint32_t>::digits;
// Seven hex digits keep the chunk radix within one word.
constexpr std::size_t DIGITS_PER_CHUNK = 7;

std::string normalizedNumber (std::string_view text, bool decimal) {
	std::string s_result;
	s_result.reserve(text.size());
	while (!text.empty()) {
		const char c_first = text.front();
		if (decimal && static_cast<unsigned char>(c_first) >= 0x80) {
			const DecimalDigit dd_digit = decimalDigit(text);
			if (dd_digit.length != 0) {
				s_result.push_back(dd_digit.ascii);
				text.remove_prefix(dd_digit.length);
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

DecimalDigit decimalDigit (std::string_view text) {
	if (text.empty()) {
		return {};
	}
	const unsigned char c_first = static_cast<unsigned char>(text.front());
	if (c_first < 0x80) {
		return c_first >= '0' && c_first <= '9' ? DecimalDigit{ static_cast<char>(c_first), 1 } : DecimalDigit{};
	}
	const std::size_t st_length = utf8_scalar_length(text);
	if (st_length == 0 || st_length > 3) {
		return {};
	}
	const char32_t c32_digit = utf8_scalar_value(text.substr(0, st_length));
	for (const char32_t c32_zero : DECIMAL_ZEROES) {
		if (c32_digit >= c32_zero && c32_digit <= c32_zero + 9) {
			return { static_cast<char>('0' + (c32_digit - c32_zero)), st_length };
		}
	}
	return {};
}

int integerLiteralRadix (std::string_view spelling) {
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
	if (token.get_type() != TokenType::IntegerLiteral) {
		throw cde::SyntaxErrors::number(token, /*floating=*/false);
	}
	std::string_view s_spelling = token.get_lexeme();
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
		const std::size_t            st_length = std::min(s_remaining.size(), DIGITS_PER_CHUNK);
		const char                  *c_end     = s_remaining.data() + st_length;
		std::uint32_t                u32_chunk;
		const std::from_chars_result fcr_chunk = std::from_chars(s_remaining.data(), c_end, u32_chunk, i_radix);
		if (fcr_chunk.ec != std::errc{} || fcr_chunk.ptr != c_end) {
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
	if (token.get_type() != TokenType::FloatingPointLiteral) {
		throw cde::SyntaxErrors::number(token, /*floating=*/true);
	}
	std::string_view s_spelling = token.get_lexeme();
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
