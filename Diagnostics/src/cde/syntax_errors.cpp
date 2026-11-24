// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <string>
#include <string_view>

#include <diagnostics.hpp>
#include <token/token.hpp>

namespace cde::SyntaxErrors {

namespace {

std::string found (const cloth::Token &token) {
	switch (token.getType()) {
	case cloth::TokenType::EndOfFile:
		return "end of file";
	case cloth::TokenType::NewLine:
		return "a newline";
	default:
		return std::format("'{}'", token.getLexeme());
	}
}

}

SyntaxException expected (std::string_view description, const cloth::Token &token) {
	return { DiagnosticCode::ExpectedToken, std::format("Expected {}, found {}", description, found(token)), token.getSpan() };
}

SyntaxException newline (const cloth::Token &token) {
	const auto semicolon = cloth::toString(cloth::Operator::OSemicolon);
	return { DiagnosticCode::ExpectedNewline, std::format("Expected a newline or '{}' after statement, found {}", semicolon, found(token)), token.getSpan(),
		 std::format("Start the next statement on a new line or separate it with '{}'.", semicolon) };
}

SyntaxException reassignment (const cloth::Token &token) {
	return { DiagnosticCode::InvalidReassignment,
		 std::format("'{}' is only allowed in a binding declaration; use '{}' for reassignment", cloth::toString(cloth::Operator::OFinalAssign),
		             cloth::toString(cloth::Operator::OAssign)),
		 token.getSpan() };
}

SyntaxException readOnlyProvider (const cloth::Token &name) {
	return { DiagnosticCode::ReadOnlyProvider, std::format("Compiler data accessor '{}' is read-only", name.getLexeme()), name.getSpan(),
		 "Copy the result into a mutable binding before changing that independent value." };
}

SyntaxException functionInType (const cloth::Token &keyword) {
	return { DiagnosticCode::FunctionInType,
		 std::format("A '{}' declaration is not allowed inside '{}'", cloth::toString(cloth::Keyword::KFn), cloth::toString(cloth::Keyword::KTypeDef)),
		 keyword.getSpan(), std::format("Declare a method with '{}' in the type body.", cloth::toString(cloth::Keyword::KMt)) };
}

SyntaxException methodOutsideType (const cloth::Token &keyword) {
	return { DiagnosticCode::MethodOutsideType,
		 std::format("A '{}' declaration requires an enclosing '{}'", cloth::toString(cloth::Keyword::KMt), cloth::toString(cloth::Keyword::KTypeDef)),
		 keyword.getSpan() };
}

SyntaxException unsupportedOperator (const cloth::Token &token) {
	return { DiagnosticCode::UnsupportedOperator, std::format("Unsupported operator '{}'", token.getLexeme()), token.getSpan() };
}

SyntaxException number (const cloth::Token &token, bool floating) {
	return { floating ? DiagnosticCode::InvalidFloat : DiagnosticCode::InvalidInteger,
		 std::format("Invalid or out-of-range {} '{}'", cloth::toString(floating ? cloth::KFloat : cloth::KInteger), token.getLexeme()),
		 token.getSpan() };
}

SyntaxException expression (const cloth::Token &token) {
	return { DiagnosticCode::ExpectedExpression, std::format("Unexpected token {}", found(token)), token.getSpan() };
}

}
