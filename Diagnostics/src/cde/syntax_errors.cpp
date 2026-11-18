// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <string>

#include <cde/syntax_errors.hpp>

namespace cde::SyntaxErrors {

namespace {

std::string found (const cloth::Token &token) {
	switch (token.get_type()) {
	case cloth::TokenType::EndOfFile:
		return "end of file";
	case cloth::TokenType::NewLine:
		return "a newline";
	default:
		return std::format("'{}'", token.get_lexeme());
	}
}

}

SyntaxException expected (std::string_view description, const cloth::Token &token) {
	return { DiagnosticCode::EXPECTED_TOKEN, std::format("Expected {}, found {}", description, found(token)), token.get_span() };
}

SyntaxException newline (const cloth::Token &token) {
	const auto semicolon = cloth::to_string(cloth::Operator::oSemicolon);
	return { DiagnosticCode::EXPECTED_NEWLINE, std::format("Expected a newline or '{}' after statement, found {}", semicolon, found(token)),
		 token.get_span(), std::format("Start the next statement on a new line or separate it with '{}'.", semicolon) };
}

SyntaxException reassignment (const cloth::Token &token) {
	return { DiagnosticCode::INVALID_REASSIGNMENT,
		 std::format("'{}' is only allowed in a binding declaration; use '{}' for reassignment", cloth::to_string(cloth::Operator::oFinalAssign),
		             cloth::to_string(cloth::Operator::oAssign)),
		 token.get_span() };
}

SyntaxException readOnlyProvider (const cloth::Token &name) {
	return { DiagnosticCode::READ_ONLY_PROVIDER, std::format("Compiler data accessor '{}' is read-only", name.get_lexeme()), name.get_span(),
		 "Copy the result into a mutable binding before changing that independent value." };
}

SyntaxException functionInType (const cloth::Token &keyword) {
	return { DiagnosticCode::FUNCTION_IN_TYPE,
		 std::format("A '{}' declaration is not allowed inside '{}'", cloth::to_string(cloth::Keyword::kFn),
		             cloth::to_string(cloth::Keyword::kTypeDef)),
		 keyword.get_span(), std::format("Declare a method with '{}' in the type body.", cloth::to_string(cloth::Keyword::kMt)) };
}

SyntaxException methodOutsideType (const cloth::Token &keyword) {
	return { DiagnosticCode::METHOD_OUTSIDE_TYPE,
		 std::format("A '{}' declaration requires an enclosing '{}'", cloth::to_string(cloth::Keyword::kMt),
		             cloth::to_string(cloth::Keyword::kTypeDef)),
		 keyword.get_span() };
}

SyntaxException unsupportedOperator (const cloth::Token &token) {
	return { DiagnosticCode::UNSUPPORTED_OPERATOR, std::format("Unsupported operator '{}'", token.get_lexeme()), token.get_span() };
}

SyntaxException number (const cloth::Token &token, bool floating) {
	return { floating ? DiagnosticCode::INVALID_FLOAT : DiagnosticCode::INVALID_INTEGER,
		 std::format("Invalid or out-of-range {} '{}'", cloth::to_string(floating ? cloth::kFloat : cloth::kInteger), token.get_lexeme()),
		 token.get_span() };
}

SyntaxException expression (const cloth::Token &token) {
	return { DiagnosticCode::EXPECTED_EXPRESSION, std::format("Unexpected token {}", found(token)), token.get_span() };
}

}
