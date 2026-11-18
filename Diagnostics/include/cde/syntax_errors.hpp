// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include <cde/syntax_exception.hpp>
#include <token/token.hpp>

namespace cde::SyntaxErrors {

SyntaxException expected (std::string_view description, const cloth::Token &token);
SyntaxException newline (const cloth::Token &token);
SyntaxException reassignment (const cloth::Token &token);
SyntaxException readOnlyProvider (const cloth::Token &name);
SyntaxException functionInType (const cloth::Token &keyword);
SyntaxException methodOutsideType (const cloth::Token &keyword);
SyntaxException unsupportedOperator (const cloth::Token &token);
SyntaxException number (const cloth::Token &token, bool floating);
SyntaxException expression (const cloth::Token &token);

}
