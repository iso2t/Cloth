// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <syntax_exception.hpp>
#include <token/token.hpp>

namespace cde::ImportErrors {

SyntaxException outsideHeader (const cloth::Token &keyword);
SyntaxException duplicateSelection (const cloth::Token &first, const cloth::Token &repeated);

}
