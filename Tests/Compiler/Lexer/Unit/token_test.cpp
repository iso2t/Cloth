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

TEST (LexerKeywords, PrivateAndPrivAreOrdinaryIdentifiers) {
	Lexer                    l_lexer("pub private priv");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();

	ASSERT_EQ(v_tokens.size(), 4u);
	expectToken(v_tokens[0], TokenType::Keyword, "pub", 1, 4);
	expectToken(v_tokens[1], TokenType::Identifier, "private", 5, 12);
	expectToken(v_tokens[2], TokenType::Identifier, "priv", 13, 17);
	expectToken(v_tokens[3], TokenType::EndOfFile, "", 17, 17);
}

TEST (LexerIdentifiers, UnicodeStartsContinuationsAndOriginalByteSpans) {
	Lexer                    l_lexer("計算 _\U00010400\u0662 e\u0301 fn\u0301 _");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();
	ASSERT_EQ(v_tokens.size(), 6u);
	expectToken(v_tokens[0], TokenType::Identifier, "計算", 1, 7);
	expectToken(v_tokens[1], TokenType::Identifier, "_\U00010400\u0662", 8, 15);
	expectToken(v_tokens[2], TokenType::Identifier, "e\u0301", 16, 19);
	EXPECT_EQ(v_tokens[2].get_name_key(), "\u00e9");
	expectToken(v_tokens[3], TokenType::Identifier, "fn\u0301", 20, 24);
	expectToken(v_tokens[4], TokenType::Operator, "_", 25, 26);
	EXPECT_EQ(v_tokens[4].get_operator(), oUnderscore);
}

TEST (LexerIdentifiers, RejectsInvalidStartsAndCharactersOutsideTheProfile) {
	for (const std::string &s_source : std::vector<std::string>{ "\u0301x", "\U0001f408", "x\u00b2", "x\xF0\x80\x80\x80" }) {
		SCOPED_TRACE(s_source);
		Lexer l_lexer(s_source);
		EXPECT_THROW(l_lexer.scanTokens(), std::runtime_error);
	}
}

TEST (LexerSpans, Utf8StringUsesByteColumns) {
	Lexer                    l_lexer("\"\xC3\xA9\" x");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();

	ASSERT_EQ(v_tokens.size(), 3u);
	expectToken(v_tokens[0], TokenType::StringLiteral, "\"\xC3\xA9\"", 1, 5);
	expectToken(v_tokens[1], TokenType::Identifier, "x", 6, 7);
	expectToken(v_tokens[2], TokenType::EndOfFile, "", 7, 7);
}

TEST (LexerSpans, LiteralAndInterpolationDelimiters) {
	Lexer                    l_lexer(R"co("""raw""" #"{{{1}}}" 'x')co");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();
	ASSERT_EQ(v_tokens.size(), 10u);
	expectToken(v_tokens[0], TokenType::RawStringLiteral, R"co("""raw""")co", 1, 10);
	expectToken(v_tokens[1], TokenType::InterpolationStart, R"co(#")co", 11, 13);
	expectToken(v_tokens[2], TokenType::StringSegment, "{{", 13, 15);
	expectToken(v_tokens[3], TokenType::InterpolationOpen, "{", 15, 16);
	expectToken(v_tokens[4], TokenType::IntegerLiteral, "1", 16, 17);
	expectToken(v_tokens[5], TokenType::InterpolationClose, "}", 17, 18);
	expectToken(v_tokens[6], TokenType::StringSegment, "}}", 18, 20);
	expectToken(v_tokens[7], TokenType::InterpolationEnd, R"co(")co", 20, 21);
	expectToken(v_tokens[8], TokenType::CharacterLiteral, "'x'", 22, 25);
	expectToken(v_tokens[9], TokenType::EndOfFile, "", 25, 25);
}

TEST (LexerKeywords, JoinsKeywordSpellingsInSourceOrder) {
	EXPECT_TRUE(to_stringmulti({}).empty());
	EXPECT_EQ(to_stringmulti({ kPublic }), "pub");
	EXPECT_EQ(to_stringmulti({ kPublic, kFinal, kTypeDef }), "pub final typedef");
}

TEST (LexerKeywords, ContextualWordsRemainIdentifiers) {
	Lexer                    l_lexer("auto intrinsic private");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();
	ASSERT_EQ(v_tokens.size(), 4u);
	expectToken(v_tokens[0], TokenType::Identifier, "auto", 1, 5);
	expectToken(v_tokens[1], TokenType::Identifier, "intrinsic", 6, 15);
	expectToken(v_tokens[2], TokenType::Identifier, "private", 16, 23);
}

TEST (LexerOperators, ResolvesBothScannedAndSynthesizedOperators) {
	Lexer                    l_lexer("+");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();
	ASSERT_EQ(v_tokens.size(), 2u);
	EXPECT_EQ(v_tokens.front().get_operator(), oPlus);

	const TokenSpan ts_span{ 1, 1, 1, 2 };
	const Token     t_operator("+", TokenType::Operator, ts_span);
	EXPECT_EQ(t_operator.get_lexeme(), "+");
	EXPECT_EQ(t_operator.get_operator(), oPlus);
	const Token t_name("longIdentifier", TokenType::Identifier, ts_span);
	EXPECT_EQ(t_name.get_lexeme(), "longIdentifier");
	EXPECT_EQ(t_name.get_operator(), oError);
}

}
}
