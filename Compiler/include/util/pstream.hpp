// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Cloth print streams
 */
#pragma once

#include <lexer/lexer.hpp>

namespace cloth {
std::string quoteLexeme (std::string_view lexeme);
void        printLexer (Lexer &lexer, std::ostream &out);
}
