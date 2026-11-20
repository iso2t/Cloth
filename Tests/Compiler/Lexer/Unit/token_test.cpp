// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

#include <cde/syntax_exception.hpp>
#include <file/source_file.hpp>
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
		EXPECT_THROW(l_lexer.scanTokens(), cde::SyntaxException);
	}
}

TEST (LexerDiagnostics, RetainsSourceAndLabelsOffendingBytes) {
	using cde::DiagnosticCode;

	struct ErrorCase {
		std::string_view source;
		DiagnosticCode   code;
		std::string_view span;
	};

	const ErrorCase cases[] = {
		{ "$", DiagnosticCode::UNEXPECTED_CHARACTER, "1:1-1:2" },
		{ "計算 \U0001F408", DiagnosticCode::UNEXPECTED_CHARACTER, "1:8-1:12" },
		{ "\n/* comment\n", DiagnosticCode::UNTERMINATED_COMMENT, "2:1-2:3" },
		{ "1__2", DiagnosticCode::INVALID_SEPARATOR, "1:2-1:3" },
		{ "\"missing", DiagnosticCode::UNTERMINATED_STRING, "1:1-1:2" },
		{ "\"\\", DiagnosticCode::UNTERMINATED_STRING, "1:1-1:2" },
		{ R"co(#"{1}tail)co", DiagnosticCode::UNTERMINATED_STRING, "1:1-1:3" },
		{ "\"\"\"\nraw", DiagnosticCode::UNTERMINATED_RAW_STRING, "1:1-1:4" },
		{ "\"\\é\"", DiagnosticCode::INVALID_ESCAPE, "1:2-1:5" },
		{ R"co(#"text\q")co", DiagnosticCode::INVALID_ESCAPE, "1:7-1:9" },
		{ "'\\q'", DiagnosticCode::INVALID_ESCAPE, "1:2-1:4" },
		{ "\"a\n", DiagnosticCode::MULTILINE_STRING, "1:3-2:1" },
		{ "#\"\\\r\n", DiagnosticCode::MULTILINE_STRING, "1:4-2:1" },
		{ "\"\xFF", DiagnosticCode::INVALID_UNICODE, "1:2-1:3" },
		{ "x\xF0\x80", DiagnosticCode::INVALID_UNICODE, "1:2-1:3" },
		{ "'", DiagnosticCode::UNTERMINATED_CHARACTER, "1:1-1:2" },
		{ "'\\", DiagnosticCode::UNTERMINATED_CHARACTER, "1:1-1:2" },
		{ "''", DiagnosticCode::EMPTY_CHARACTER, "1:1-1:3" },
		{ "'ab'", DiagnosticCode::INVALID_CHARACTER_LENGTH, "1:1-1:3" },
		{ R"co(#"a {#"b {1)co", DiagnosticCode::UNTERMINATED_INTERPOLATION, "1:10-1:11" },
		{ R"co(#"ok })co", DiagnosticCode::UNMATCHED_INTERPOLATION_BRACE, "1:6-1:7" },
		{ "0b2", DiagnosticCode::INVALID_NUMERIC_LITERAL, "1:1-1:4" },
	};
	for (const ErrorCase &test : cases) {
		SCOPED_TRACE(test.source);
		try {
			Lexer{ SourceFile{ "fixture.co", std::string(test.source) } }.scan();
			FAIL() << "Expected a lexer diagnostic";
		} catch (const cde::SyntaxException &error) {
			EXPECT_EQ(error.get_code(), test.code);
			ASSERT_EQ(error.get_source_labels().size(), 1u);
			const TokenSpan &ts_primary = error.get_source_labels().front().span;
			ASSERT_NE(ts_primary.source, nullptr);
			EXPECT_EQ(ts_primary.source->get_path(), "fixture.co");
			EXPECT_EQ(ts_primary.source->get_contents(), test.source);
			EXPECT_EQ(ts_primary.to_string(), test.span);
		}
	}
}

TEST (LexerDiagnostics, OffsetScanUsesOriginalFileCoordinates) {
	const std::string s_prefix = "$ skipped\r\n";
	const SourceFile  sf_file{ "offset.co", s_prefix + "計算 \U0001F408" };
	try {
		Lexer{ sf_file, s_prefix.size() }.scan();
		FAIL() << "Expected a lexer diagnostic";
	} catch (const cde::SyntaxException &error) {
		ASSERT_EQ(error.get_code(), cde::DiagnosticCode::UNEXPECTED_CHARACTER);
		ASSERT_EQ(error.get_source_labels().size(), 1u);
		const TokenSpan &ts_primary = error.get_source_labels().front().span;
		ASSERT_NE(ts_primary.source, nullptr);
		EXPECT_EQ(ts_primary.source->get_path(), sf_file.get_path());
		EXPECT_EQ(ts_primary.source->get_contents(), sf_file.get_contents());
		EXPECT_EQ(ts_primary.to_string(), "2:8-2:12");
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

TEST (LexerSpans, ValidEscapesInEachLiteralKind) {
	Lexer                    l_lexer(R"co("é\n" #"\t{{x}}" '\\')co");
	const std::vector<Token> v_tokens = l_lexer.scanTokens();
	ASSERT_EQ(v_tokens.size(), 6u);
	expectToken(v_tokens[0], TokenType::StringLiteral, R"co("é\n")co", 1, 7);
	expectToken(v_tokens[2], TokenType::StringSegment, R"co(\t{{x}})co", 10, 17);
	expectToken(v_tokens[4], TokenType::CharacterLiteral, R"co('\\')co", 19, 23);
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
