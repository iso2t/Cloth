// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <diagnostics.hpp>
#include <token/token.hpp>

namespace cde::GenericErrors {

SyntaxException syntax (std::string_view feature, const cloth::Token &token) {
	return { DiagnosticCode::UnsupportedGenericSyntax, std::format("{} are outside the supported generic syntax", feature), token.getSpan() };
}

SyntaxException nesting (const cloth::TokenSpan &span, std::size_t limit) {
	return { DiagnosticCode::GenericArgumentNesting, std::format("Generic type argument nesting limit exceeded ({})", limit), span };
}

ClothDiagnostic argument (std::string message, const cloth::TokenSpan &span) {
	return { DiagnosticCode::InvalidGenericArgument, std::move(message), { { .span = span, .text = "invalid type argument here" } } };
}

}
