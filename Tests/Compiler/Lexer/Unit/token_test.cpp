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
#include "cde/diagnostic_code.hpp"

namespace cloth {
namespace {

void expectToken (const Token &token, TokenType type, std::string_view spelling, std::size_t startColumn, std::size_t endColumn) {
	SCOPED_TRACE(::testing::Message() << "Token at column " << startColumn);
	EXPECT_EQ(token.getType(), type);
	EXPECT_EQ(token.getLexeme(), spelling);
	const TokenSpan ts_span = token.getSpan();
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
	EXPECT_EQ(v_tokens[2].getNameKey(), "\u00e9");
	expectToken(v_tokens[3], TokenType::Identifier, "fn\u0301", 20, 24);
	expectToken(v_tokens[4], TokenType::Operator, "_", 25, 26);
	EXPECT_EQ(v_tokens[4].getOperator(), OUnderscore);
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
		{ .source="$", .code=DiagnosticCode::UnexpectedCharacter, .span="1:1-1:2" },
		{ .source="計算 \U0001F408", .code=DiagnosticCode::UnexpectedCharacter, .span="1:8-1:12" },
		{ .source="\n/* comment\n", .code=DiagnosticCode::UnterminatedComment, .span="2:1-2:3" },
		{ .source="1__2", .code=DiagnosticCode::InvalidSeparator, .span="1:2-1:3" },
		{ .source="\"missing", .code=DiagnosticCode::UnterminatedString, .span="1:1-1:2" },
		{ .source="\"\\", .code=DiagnosticCode::UnterminatedString, .span="1:1-1:2" },
		{ .source=R"co(#"{1}tail)co", .code=DiagnosticCode::UnterminatedString, .span="1:1-1:3" },
		{ .source="\"\"\"\nraw", .code=DiagnosticCode::UnterminatedRawString, .span="1:1-1:4" },
		{ .source="\"\\é\"", .code=DiagnosticCode::InvalidEscape, .span="1:2-1:5" },
		{ .source=R"co(#"text\q")co", .code=DiagnosticCode::InvalidEscape, .span="1:7-1:9" },
		{ .source="'\\q'", .code=DiagnosticCode::InvalidEscape, .span="1:2-1:4" },
		{ .source="\"a\n", .code=DiagnosticCode::MultilineString, .span="1:3-2:1" },
		{ .source="#\"\\\r\n", .code=DiagnosticCode::MultilineString, .span="1:4-2:1" },
		{ .source="\"\xFF", .code=DiagnosticCode::InvalidUnicode, .span="1:2-1:3" },
		{ .source="x\xF0\x80", .code=DiagnosticCode::InvalidUnicode, .span="1:2-1:3" },
		{ .source="'", .code=DiagnosticCode::UnterminatedCharacter, .span="1:1-1:2" },
		{ .source="'\\", .code=DiagnosticCode::UnterminatedCharacter, .span="1:1-1:2" },
		{ .source="''", .code=DiagnosticCode::EmptyCharacter, .span="1:1-1:3" },
		{ .source="'ab'", .code=DiagnosticCode::InvalidCharacterLength, .span="1:1-1:3" },
		{ .source=R"co(#"a {#"b {1)co", .code=DiagnosticCode::UnterminatedInterpolation, .span="1:10-1:11" },
		{ .source=R"co(#"ok })co", .code=DiagnosticCode::UnmatchedInterpolationBrace, .span="1:6-1:7" },
		{ .source="0b2", .code=DiagnosticCode::InvalidNumericLiteral, .span="1:1-1:4" },
	};
	for (const ErrorCase &test : cases) {
		SCOPED_TRACE(test.source);
		try {
			Lexer{ SourceFile{ "fixture.co", std::string(test.source) } }.scan();
			FAIL() << "Expected a lexer diagnostic";
		} catch (const cde::SyntaxException &error) {
			EXPECT_EQ(error.getCode(), test.code);
			ASSERT_EQ(error.getSourceLabels().size(), 1u);
			const TokenSpan &ts_primary = error.getSourceLabels().front().span;
			ASSERT_NE(ts_primary.source, nullptr);
			EXPECT_EQ(ts_primary.source->getPath(), "fixture.co");
			EXPECT_EQ(ts_primary.source->getContents(), test.source);
			EXPECT_EQ(ts_primary.toString(), test.span);
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
		ASSERT_EQ(error.getCode(), cde::DiagnosticCode::UnexpectedCharacter);
		ASSERT_EQ(error.getSourceLabels().size(), 1u);
		const TokenSpan &ts_primary = error.getSourceLabels().front().span;
		ASSERT_NE(ts_primary.source, nullptr);
		EXPECT_EQ(ts_primary.source->getPath(), sf_file.getPath());
		EXPECT_EQ(ts_primary.source->getContents(), sf_file.getContents());
		EXPECT_EQ(ts_primary.toString(), "2:8-2:12");
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
	EXPECT_TRUE(toStringmulti({}).empty());
	EXPECT_EQ(toStringmulti({ KPublic }), "pub");
	EXPECT_EQ(toStringmulti({ KPublic, KFinal, KTypeDef }), "pub final typedef");
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
	EXPECT_EQ(v_tokens.front().getOperator(), OPlus);

	const TokenSpan ts_span{ .startLine=1, .startCol=1, .endLine=1, .endCol=2 };
	const Token     t_operator("+", TokenType::Operator, ts_span);
	EXPECT_EQ(t_operator.getLexeme(), "+");
	EXPECT_EQ(t_operator.getOperator(), OPlus);
	const Token t_name("longIdentifier", TokenType::Identifier, ts_span);
	EXPECT_EQ(t_name.getLexeme(), "longIdentifier");
	EXPECT_EQ(t_name.getOperator(), OError);
}

}
}
