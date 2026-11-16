// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/import_declaration.hpp>
#include <parser/parser.hpp>
#include <token/token.hpp>

namespace cloth {

#define NOT_IMPLEMENTED throw std::logic_error("Not Implemented!");

/**
 * I may have over engineered this.
 * Peeks the next token at a given offset.
 * The offset defaults to the next up token.
 */
Token Parser::peek (int offset) {
	if (v_tokens.empty()) throw std::out_of_range("Token stream is empty.");

	const std::int64_t i64_target = static_cast<std::int64_t>(i_currentIndex) + offset;
	if (i64_target < 0) return v_tokens.front();

	if (static_cast<std::uint64_t>(i64_target) >= v_tokens.size()) return v_tokens.back();

	return v_tokens[static_cast<std::size_t>(i64_target)];
}

Token Parser::previous () {
	return v_tokens[i_currentIndex - 1];
}

Token Parser::advance () {
	return v_tokens[i_currentIndex++];
}

SourceUnit Parser::parse () {
	return parseDeclarations();
}

SourceUnit Parser::parseDeclarations () {
	std::vector<Declaration>       v_declarations;
	std::vector<ImportDeclaration> v_imports;
	skipNewLines();

	while (peek().get_type() != TokenType::EndOfFile) {
		if (atKeyword(Keyword::k_USING)) {
			if (!v_declarations.empty()) throw std::logic_error("Outside of header!");
			v_imports.push_back(importParser.parse());
			skipNewLines();
			continue;
		}
	}
}

std::unique_ptr<bool> Parser::startsTypeDeclaration (){ NOT_IMPLEMENTED }

std::unique_ptr<TypeDeclaration> Parser::parseNestedTypeDeclaration(){ NOT_IMPLEMENTED }

Declaration Parser::parseTypeDeclaration(bool nested) {
	NOT_IMPLEMENTED
}

bool Parser::atKeyword (Keyword keyword, int offset) {
	Token t_offset = peek(offset);
	return t_offset.get_type() == TokenType::Keyword && t_offset.get_lexeme() == cloth::to_string(keyword);
}

void Parser::rejectMethodOutsideType () {
	int i_offset = atKeyword(Keyword::k_PUBLIC) ? 1 : 0;
	if (atKeyword(Keyword::k_MT, i_offset)) {
		throw std::logic_error("Method outside type");
	}
}

BlockBody Parser::parseBlock (){ NOT_IMPLEMENTED }

std::unique_ptr<Expression> Parser::parseBindingBrace(bool allowLiteral, bool allowSingleField,
                                                      std::function<std::unique_ptr<FunctionBlockExpression>(BlockBody)> deferred){ NOT_IMPLEMENTED }

std::vector<Statement> Parser::parseStatements(std::optional<BindingBraceSyntax> braceSyntax) {
	NOT_IMPLEMENTED
}

void Parser::skipNewLines () {
	while (peek().get_type() == TokenType::NewLine) advance();
}

bool Parser::isSemiColon (Token token) {
	return token.get_type() == TokenType::Operator && token.get_lexeme() == cloth::to_string(Operator::o_SEMICOLON);
}

bool Parser::isStatementSeparator (Token token) {
	return token.get_type() == TokenType::NewLine || isSemiColon(token);
}

Statement Parser::parseStatement (bool allowWhileTrailer, bool multilineResults){ NOT_IMPLEMENTED }

BlockBody Parser::parseUnbracedBody(bool allowWhileTrailer){ NOT_IMPLEMENTED }

TokenSpan Parser::previousStatementEnd() {
	int i_index = i_currentIndex - 1;
	while (isStatementSeparator(v_tokens[i_index])) i_index--;
	return v_tokens[i_index].get_span();
}

ReturnStatement Parser::returnStatement (bool allowWhileTrailer) {
	NOT_IMPLEMENTED
}

bool Parser::startsAssignmentList () {
	if (!isReceivingTarget(peek(), true)) return false;
	int i_offset = 1;
	while (peek(i_offset).get_lexeme() == cloth::to_string(Operator::o_COMMA)) {
		if (!isReceivingTarget(peek(i_offset + 1), true)) return false;
		i_offset += 2;
	}

	static const std::unordered_set<std::string> assignOps = { cloth::to_string(Operator::o_ASSIGN), cloth::to_string(Operator::o_FINAL_ASSIGN) };
	return (i_offset > 1 || peek().get_lexeme() == cloth::to_string(Operator::o_UNDERSCORE)) && assignOps.contains(peek(i_offset).get_lexeme());
}

bool Parser::isReceivingTarget (Token token, bool assignment) {
	return [&] () {
		switch (token.get_type()) {
		case cloth::TokenType::Identifier:
			return true;
		case cloth::TokenType::Operator:
			return token.get_lexeme() == cloth::to_string(Operator::o_UNDERSCORE);
		case cloth::TokenType::Keyword:
			return assignment && token.get_lexeme() == cloth::to_string(Keyword::k_SELF);
		default:
			return false;
		}
	}();
}

ReceivingTarget Parser::parseReceivingTarget (bool assignment){ NOT_IMPLEMENTED }

Expression Parser::parseResults(std::optional<Expression> first, TokenSpan span, bool allowWhileTrailer, bool multiline){ NOT_IMPLEMENTED }

std::vector<TypeReference> Parser::parseTypeList(){ NOT_IMPLEMENTED }

Token Parser::expect(TokenType type, std::string description) {
	Token t_token = peek();
	if (t_token.get_type() != type) {
		std::string s_tokenStr = t_token.to_string();
		throw std::logic_error(std::vformat(description, std::make_format_args(s_tokenStr)));
	}
	return advance();
}

Token Parser::expect (Operator op) {
	Token t_token = peek();
	if (t_token.get_type() != TokenType::Operator && t_token.get_lexeme() != cloth::to_string(op)) {
		std::string s_tokenStr = t_token.to_string();
		throw std::logic_error(std::vformat("'{}'", std::make_format_args(s_tokenStr)));
	}
	return advance();
}

} // namespace cloth
