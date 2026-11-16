// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/import_declaration.hpp>
#include <parser/parser.hpp>
#include <token/token.hpp>

namespace cloth {

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

bool Parser::startsTypeDeclaration () {
	return false;
}

void Parser::skipNewLines () {
	while (peek().get_type() == TokenType::NewLine) advance();
}
} // namespace cloth
