// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <utility>

#include <cde/generic_errors.hpp>

namespace cde::GenericErrors {

SyntaxException syntax (std::string_view feature, const cloth::Token &token) {
	return { DiagnosticCode::UNSUPPORTED_GENERIC_SYNTAX, std::format("{} are outside the supported generic syntax", feature), token.get_span() };
}

SyntaxException nesting (const cloth::TokenSpan &span, std::size_t limit) {
	return { DiagnosticCode::GENERIC_ARGUMENT_NESTING, std::format("Generic type argument nesting limit exceeded ({})", limit), span };
}

ClothDiagnostic argument (std::string message, const cloth::TokenSpan &span) {
	return { DiagnosticCode::INVALID_GENERIC_ARGUMENT, std::move(message), { { span, "invalid type argument here" } } };
}

}
