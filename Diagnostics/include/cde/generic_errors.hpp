// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <cde/cloth_diagnostic.hpp>
#include <cde/syntax_exception.hpp>
#include <token/token.hpp>

namespace cde::GenericErrors {

SyntaxException syntax (std::string_view feature, const cloth::Token &token);
SyntaxException nesting (const cloth::TokenSpan &span, std::size_t limit);
ClothDiagnostic argument (std::string message, const cloth::TokenSpan &span);

}
