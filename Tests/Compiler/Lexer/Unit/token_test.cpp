// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

#include <lexer/lexer.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

void expectToken (const Token &token, TokenType type, std::string_view spelling, std::size_t startColumn, std::size_t endColumn) {
	SCOPED_TRACE(::testing::Message() << "Token at column " << startColumn);
	EXPECT_EQ(token.get_type(), type);
	EXPECT_EQ(token.get_lexeme(), spelling);
	const TokenSpan ts_span = token.get_span();
	EXPECT_EQ(ts_span.startLine, 1u);
	EXPECT_EQ(ts_span.startCol, startColumn);
	EXPECT_EQ(ts_span.endLine, 1u);
	EXPECT_EQ(ts_span.endCol, endColumn);
}

TEST (LexerKeywords, PubAndBoolInFunctionSignature) {
	Lexer                    l_lexer("pub fn<bool> ready() = true");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();

	ASSERT_EQ(v_tokens.size(), 11u);
	expectToken(v_tokens[0], TokenType::Keyword, "pub", 1, 4);
	expectToken(v_tokens[1], TokenType::Keyword, "fn", 5, 7);
	expectToken(v_tokens[2], TokenType::Operator, "<", 7, 8);
	expectToken(v_tokens[3], TokenType::Keyword, "bool", 8, 12);
	expectToken(v_tokens[4], TokenType::Operator, ">", 12, 13);
	expectToken(v_tokens[5], TokenType::Identifier, "ready", 14, 19);
	expectToken(v_tokens[6], TokenType::Operator, "(", 19, 20);
	expectToken(v_tokens[7], TokenType::Operator, ")", 20, 21);
	expectToken(v_tokens[8], TokenType::Operator, "=", 22, 23);
	expectToken(v_tokens[9], TokenType::Keyword, "true", 24, 28);
	expectToken(v_tokens[10], TokenType::EndOfFile, "", 28, 28);
}

TEST (LexerSpans, Utf8StringUsesByteColumns) {
	Lexer                    l_lexer("\"\xC3\xA9\" x");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();

	ASSERT_EQ(v_tokens.size(), 3u);
	expectToken(v_tokens[0], TokenType::StringLiteral, "\"\xC3\xA9\"", 1, 5);
	expectToken(v_tokens[1], TokenType::Identifier, "x", 6, 7);
	expectToken(v_tokens[2], TokenType::EndOfFile, "", 7, 7);
}

}
}
