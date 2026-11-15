// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Cloth print streams
 */
#include <ostream>
#include <lexer/Lexer.hpp>

namespace cloth {
std::string quoteLexeme(std::string_view lexeme);
void printLexer(Lexer &lexer, std::ostream &out);
}
