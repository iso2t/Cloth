// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <token/token.hpp>

namespace cloth {

struct DecimalDigit {
	char        ascii;
	std::size_t length;
};

DecimalDigit decimalDigit (std::string_view text);
int          integerLiteralRadix (std::string_view spelling);

/** Unsigned magnitude in least-significant-word-first order. Zero has one zero word. */
using IntegerValue = std::vector<std::uint32_t>;

IntegerValue integerLiteralValue (const Token &token);
std::string  floatingLiteralText (const Token &token);

}
