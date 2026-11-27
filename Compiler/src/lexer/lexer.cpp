// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/source/Source.rst

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <diagnostics.hpp>
#include <lexer/lexer.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>
#include <util/unicode_identifier.hpp>
#include <util/utf8.hpp>

namespace cloth {
namespace {

using cde::DiagnosticCode;

const std::string_view S_STRING_QUOTE         = toString(ODoubleQuotation);
const std::string_view S_CHARACTER_QUOTE      = toString(OQuotation);
const std::string_view S_INTERPOLATION_OPEN   = toString(OBraceOpen);
const std::string_view S_INTERPOLATION_CLOSE  = toString(OBraceClose);
const std::string      S_INTERPOLATION_PREFIX = std::format("{}{}", toString(OPound), S_STRING_QUOTE);

/**
 * @brief Represents an entry for an operator, including its type and spelling.
 *
 * @param op The operator type.
 * @param spelling The spelling of the operator.
 * @return OperatorEntry The operator entry containing the type and spelling.
 */
struct OperatorEntry {
	Operator         op;
	std::string_view spelling;
};

/**
 * @brief Returns a list of all operators sorted by descending spelling length.
 *
 * @return std::vector<OperatorEntry> The list of operators sorted by descending
 * spelling length.
 */
const std::vector<OperatorEntry> V_OPERATORS = [] {
	std::vector<OperatorEntry> v_entries;

	for (int i_value = OPlus; i_value < OError; ++i_value) {
		if (const auto o_operator = static_cast<Operator>(i_value); isTokenized(o_operator)) {
			v_entries.push_back({ .op = o_operator, .spelling = toString(o_operator) });
		}
	}

	std::ranges::sort(v_entries, [] (const OperatorEntry &left, const OperatorEntry &right) { return left.spelling.size() > right.spelling.size(); });

	return v_entries;
}();

// Contextual keywords keep their identifier token outside the grammar that recognizes them.
const std::vector<Keyword> V_KEYWORDS = [] {
	std::vector<Keyword> v_entries;

	for (int i_value = KMod; i_value < KError; ++i_value) {
		if (const auto k_keyword = static_cast<Keyword>(i_value); isReserved(k_keyword)) {
			v_entries.push_back(k_keyword);
		}
	}

	return v_entries;
}();

/**
 * @brief Returns the character at the specified index in the source, or '\0' if
 * out of bounds.
 *
 * @param source The source string to check.
 * @param index The index of the character to retrieve.
 * @return char The character at the specified index, or '\0' if out of bounds.
 */
char charAt (const std::string_view source, const std::size_t index) {
	return index < source.size() ? source[index] : '\0';
}

/**
 * @brief Checks if the given character is a digit.
 *
 * @param c The character to check.
 * @return true If the character is a digit.
 * @return false Otherwise.
 */
bool isDigit (const char c) {
	return c >= '0' && c <= '9';
}

std::size_t identifierPartLength (const std::string_view source, const std::size_t offset) {
	return offset < source.size() ? identifierContinueLength(source.substr(offset)) : 0;
}

std::size_t radixDigitLength (const std::string_view source, const std::size_t offset, const int radix) {
	if (radix == 10) {
		return decimalDigit(source.substr(offset)).length;
	}
	const char c_digit = charAt(source, offset);
	if (isDigit(c_digit)) {
		return c_digit - '0' < radix ? 1 : 0;
	}
	return radix == 16 && ((c_digit >= 'a' && c_digit <= 'f') || (c_digit >= 'A' && c_digit <= 'F')) ? 1 : 0;
}

/**
 * @brief Checks if the given character is a line break character.
 *
 * @param c The character to check.
 * @return true If the character is a line break character.
 * @return false Otherwise.
 */
bool isLineBreak (const char c) {
	return c == '\n' || c == '\r';
}

/**
 * @brief Checks if the given character is a valid escape sequence character.
 *
 * @param c The character to check.
 * @return true If the character is a valid escape sequence character.
 * @return false Otherwise.
 */
bool isValidEscape (const char c) {
	return c == S_STRING_QUOTE.front() || c == S_CHARACTER_QUOTE.front() || std::string_view{ "nrt0\\" }.find(c) != std::string_view::npos;
}

} // namespace

Lexer::Lexer (const SourceFile &source, const std::size_t offset) : Lexer(source) {
	if (offset > sourceText().size()) throw std::out_of_range("Lexer offset exceeds the source length");
	advanceBytes(offset);
}

/**
 * @brief Advances the lexer by one source byte and updates the current position.
 *
 * @return char The consumed byte.
 */
char Lexer::advance () {
	const char c = sourceText()[st_current++];
	// The LF completes CRLF, so an offset between its bytes still belongs to the preceding line.
	if (c == '\n' || (c == '\r' && peek() != '\n')) {
		p_current = { .line = p_current.line + 1, .column = 1 };
	} else {
		++p_current.column;
	}
	return c;
}

void Lexer::advanceBytes (const std::size_t length) {
	for (std::size_t st_byte = 0; st_byte < length; ++st_byte) {
		advance();
	}
}

bool Lexer::consumeLineBreak () {
	if (!isLineBreak(peek())) return false;
	if (advance() == '\r' && peek() == '\n') advance();
	return true;
}

/**
 * @brief Returns the current character without advancing the lexer.
 *
 * @return char The current character without advancing the lexer.
 */
char Lexer::peek () const {
	return charAt(sourceText(), st_current);
}

bool Lexer::startsWith (const std::string_view spelling) const {
	return std::string_view(sourceText()).substr(st_current).starts_with(spelling);
}

/**
 * @brief Checks if the lexer has reached the end of the source.
 *
 * @return true If the lexer is at the end of the source.
 * @return false Otherwise.
 */
bool Lexer::isAtEnd () const {
	return st_current >= sourceText().size();
}

/**
 * @brief Scans the source and returns a vector of tokens.
 *
 * @return std::vector<Token> The scanned tokens.
 */
std::vector<Token> Lexer::scanTokens () {
	scan();
	return v_tokens;
}

void Lexer::scan () {
	while (!isAtEnd()) {
		st_start = st_current;
		p_start  = p_current;
		scanToken();
	}

	if (v_tokens.empty() || v_tokens.back().getType() != TokenType::EndOfFile) {
		addToken(TokenType::EndOfFile, spanFrom(p_current), "");
	}
}

/**
 * @brief Adds a token to the list of scanned tokens.
 *
 * @param type The type of the token.
 * @param span The span of the token in the source.
 * @param lexeme The lexeme of the token.
 * @return void Always returns void; adds the token to the list of scanned
 * tokens.
 */
void Lexer::addToken (TokenType type, const TokenSpan &span, const std::string &lexeme) {
	v_tokens.emplace_back(lexeme, type, span);
}

/**
 * @brief Returns the span of the current token.
 *
 * @return TokenSpan The span of the current token.
 */
TokenSpan Lexer::span () const {
	return spanFrom(p_start);
}

TokenSpan Lexer::spanFrom (const Position start) const {
	return { .startLine = start.line, .startCol = start.column, .endLine = p_current.line, .endCol = p_current.column, .source = p_source };
}

/**
 * @brief Matches the next operator in the source.
 *
 * @return Operator The matched operator, or o_ERROR if no
 * operator matches.
 */
Operator Lexer::matchOperator () {
	const std::string_view s_remaining = std::string_view{ sourceText() }.substr(st_current);

	for (const auto &[op, spelling] : V_OPERATORS) {
		if (!s_remaining.starts_with(spelling)) {
			continue;
		}

		// A nullable type followed by provider access begins with the same bytes as the fallback operator.
		if (op == OElvis && s_remaining.substr(toString(OQuestion).size()).starts_with(toString(OColonColon))) continue;
		if (op == OUnderscore && identifierPartLength(s_remaining, spelling.size()) != 0) {
			continue;
		}

		for (std::size_t i = 0; i < spelling.size(); ++i) {
			advance();
		}
		return op;
	}

	return OError;
}

/**
 * @brief Scans the next token from the source and adds it to the list of
 * tokens.
 * @return void
 */
void Lexer::scanToken () {
	if (consumeLineBreak()) {
		addToken(TokenType::NewLine, span(), "\n");
		return;
	}
	const std::string_view s_remaining = std::string_view{ sourceText() }.substr(st_current);

	if (s_remaining.starts_with(S_INTERPOLATION_PREFIX)) {
		advanceBytes(S_INTERPOLATION_PREFIX.size());
		addToken(TokenType::InterpolationStart, span(), sourceText().substr(st_start, st_current - st_start));
		interpolatedString();
		return;
	}

	if (s_remaining.starts_with(rawStringDelimiter())) {
		advanceBytes(rawStringDelimiter().size());
		rawString();
		return;
	}

	if (s_remaining.starts_with(S_STRING_QUOTE)) {
		advanceBytes(S_STRING_QUOTE.size());
		string();
		return;
	}

	if (s_remaining.starts_with(S_CHARACTER_QUOTE)) {
		advanceBytes(S_CHARACTER_QUOTE.size());
		character();
		return;
	}

	if (skipComments()) {
		return;
	}

	if (const auto op = matchOperator(); op != OError) {
		v_tokens.emplace_back(sourceText().substr(st_start, st_current - st_start), op, span());
		return;
	}

	if (decimalDigit(s_remaining).length != 0) {
		number();
		return;
	}

	if (const std::size_t st_start = identifierStartLength(s_remaining); st_start != 0) {
		advanceBytes(st_start);
		identifier();
		return;
	}

	switch (const char c_character = advance()) {
	case ' ':
	case '\t':
		return;
	default: {
		const std::size_t st_size = utf8ScalarLength(s_remaining);
		if (st_size > 1) {
			const char32_t c32_scalar = utf8ScalarValue(s_remaining.substr(0, st_size));
			advanceBytes(st_size - 1);
			throw cde::SyntaxException{ DiagnosticCode::UnexpectedCharacter,
				                    std::format("Unexpected character U+{:04X}", static_cast<unsigned>(c32_scalar)), span() };
		}
		throw cde::SyntaxException{ st_size == 0 ? DiagnosticCode::InvalidUnicode : DiagnosticCode::UnexpectedCharacter,
			                    std::format("Unexpected byte 0x{:02X}", static_cast<unsigned char>(c_character)), span() };
	}
	}
}

/**
 * @brief Skips comments in the source code.
 *
 * @return true If a comment was skipped.
 * @return false If no comment was found.
 */
bool Lexer::skipComments () {
	if (peek() != '/') {
		return false;
	}

	switch (charAt(sourceText(), st_current + 1)) {
	case '/':
		advance();
		advance();
		while (!isAtEnd() && !isLineBreak(peek())) {
			advance();
		}
		return true;

	case '*': {
		advance();
		advance();
		const TokenSpan ts_opening = span();
		while (!isAtEnd()) {
			if (peek() == '*' && charAt(sourceText(), st_current + 1) == '/') {
				advance();
				advance();
				return true;
			}
			advance();
		}
		throw cde::SyntaxException{ DiagnosticCode::UnterminatedComment, "Unterminated block comment", ts_opening };
	}

	default:
		return false;
	}
}

/**
 * @brief Scans a numeric literal from the source code.
 * Handles integer and floating-point literals with optional radix prefixes and
 * suffixes.
 *
 * @return void Always returns void; adds the numeric token to the list of
 * tokens.
 */
void Lexer::number () {
	const std::string_view s_source  = sourceText();
	const int              i_radix   = integerLiteralRadix(s_source.substr(st_current));
	bool                   b_isFloat = false;

	if (i_radix != 10) {
		advanceBytes(2);
		if (peek() == '_') {
			invalidNumericSeparator();
		}
		if (radixDigitLength(s_source, st_current, i_radix) == 0) {
			invalidNumber(std::format("Expected a base-{} digit after the prefix", i_radix));
		}
	}
	consumeNumericDigits(i_radix);

	if (peek() == '.') {
		const char c_next           = charAt(s_source, st_current + 1);
		const bool b_completeSuffix = identifierPartLength(s_source, st_current + 2) == 0;
		const bool b_trailingFloat  = b_completeSuffix && (c_next == 'f' || c_next == 'F');
		if (const bool b_trailingInteger = b_completeSuffix && (c_next == 'i' || c_next == 'I');
		    decimalDigit(s_source.substr(st_current + 1)).length != 0 || b_trailingFloat || b_trailingInteger || c_next == '_') {
			advance();
			if (i_radix != 10) {
				invalidNumber("Base-prefixed literals cannot have a decimal point or suffix");
			}
			if (b_trailingInteger) {
				invalidNumber("An integer suffix cannot follow a decimal point");
			}
			if (peek() == '_') {
				invalidNumericSeparator();
			}
			b_isFloat = true;
			consumeNumericDigits(i_radix);
		}
	}

	if (const char suffix = peek(); i_radix == 10 && (suffix == 'f' || suffix == 'F' || suffix == 'i' || suffix == 'I')) {
		advance();
		const bool floatSuffix = suffix == 'f' || suffix == 'F';
		if (b_isFloat && !floatSuffix) {
			invalidNumber("An integer suffix cannot follow a decimal point");
		}
		b_isFloat = b_isFloat || floatSuffix;
	}

	if (identifierPartLength(s_source, st_current) != 0) {
		invalidNumber(i_radix == 10 ? "Invalid numeric suffix" : std::format("Invalid base-{} digit or forbidden suffix", i_radix));
	}

	addToken(b_isFloat ? TokenType::FloatingPointLiteral : TokenType::IntegerLiteral, span(), sourceText().substr(st_start, st_current - st_start));
}

void Lexer::consumeNumericDigits (const int radix) {
	const std::string_view s_source = sourceText();
	for (std::size_t st_length = radixDigitLength(s_source, st_current, radix); st_length != 0; st_length = radixDigitLength(s_source, st_current, radix)) {
		advanceBytes(st_length);
		if (peek() == '_') {
			if (radixDigitLength(s_source, st_current + 1, radix) == 0) {
				invalidNumericSeparator();
			}
			advance();
		}
	}
}

void Lexer::invalidNumber (std::string_view reason) {
	consumeIdentifierParts();
	throw cde::SyntaxException{ DiagnosticCode::InvalidNumericLiteral,
		                    std::format("Invalid numeric literal '{}': {}", sourceText().substr(st_start, st_current - st_start), reason), span() };
}

void Lexer::invalidNumericSeparator () const {
	TokenSpan ts_separator = spanFrom(p_current);
	++ts_separator.endCol;
	throw cde::SyntaxException{ DiagnosticCode::InvalidSeparator, "Numeric separators must appear between digits", ts_separator };
}

void Lexer::consumeIdentifierParts () {
	const std::string_view s_source = sourceText();
	for (std::size_t st_length = identifierPartLength(s_source, st_current); st_length != 0; st_length = identifierPartLength(s_source, st_current)) {
		advanceBytes(st_length);
	}
}

/**
 * @brief Scans an identifier from the source code.
 *
 * Handles keywords and regular identifiers.
 *
 * @return void Always returns void; adds the identifier token to the list of
 * tokens.
 *
 */
void Lexer::identifier () {
	consumeIdentifierParts();

	const std::string s_text      = sourceText().substr(st_start, st_current - st_start);
	const bool        b_isKeyword = std::ranges::any_of(V_KEYWORDS, [&s_text] (const Keyword keyword) { return toString(keyword) == s_text; });

	addToken(b_isKeyword ? TokenType::Keyword : TokenType::Identifier, span(), s_text);
}

/**
 * @brief Scans an interpolated string from the source code.
 * Handles string segments, interpolation holes, and escape sequences.
 *
 * @return void Always returns void; adds the interpolated string tokens to the
 * list of tokens.
 */
void Lexer::interpolatedString () {
	const TokenSpan ts_opening = span();
	const auto      mark       = [this] {
                st_start = st_current;
                p_start  = p_current;
	};
	const auto emit = [this] (const TokenType type) { addToken(type, span(), sourceText().substr(st_start, st_current - st_start)); };

	mark();
	while (!isAtEnd()) {
		if (startsWith(S_STRING_QUOTE)) {
			emit(TokenType::StringSegment);
			mark();
			advanceBytes(S_STRING_QUOTE.size());
			emit(TokenType::InterpolationEnd);
			return;
		}

		if (const bool b_open = startsWith(S_INTERPOLATION_OPEN); b_open || startsWith(S_INTERPOLATION_CLOSE)) {
			const std::string_view s_brace = b_open ? S_INTERPOLATION_OPEN : S_INTERPOLATION_CLOSE;
			if (std::string_view(sourceText()).substr(st_current + s_brace.size()).starts_with(s_brace)) {
				advanceBytes(2 * s_brace.size());
				continue;
			}
			if (!b_open) {
				mark();
				advanceBytes(s_brace.size());
				throw cde::SyntaxException{ DiagnosticCode::UnmatchedInterpolationBrace,
					                    std::format("Expected '{0}{0}' for a literal closing brace", s_brace), span() };
			}

			emit(TokenType::StringSegment);
			mark();
			advanceBytes(S_INTERPOLATION_OPEN.size());
			emit(TokenType::InterpolationOpen);
			const TokenSpan ts_hole = span();
			int             depth   = 0;
			while (!isAtEnd()) {
				mark();
				if (startsWith(S_INTERPOLATION_CLOSE)) {
					if (depth == 0) {
						break;
					}
					--depth;
				} else if (startsWith(S_INTERPOLATION_OPEN)) {
					++depth;
				}
				scanToken();
			}
			if (isAtEnd()) {
				throw cde::SyntaxException{ DiagnosticCode::UnterminatedInterpolation, "Unterminated interpolation", ts_hole };
			}

			advanceBytes(S_INTERPOLATION_CLOSE.size());
			emit(TokenType::InterpolationClose);
			mark();
			continue;
		}

		advanceStringCharacter("interpolated string");
	}

	throw cde::SyntaxException{ DiagnosticCode::UnterminatedString, "Unterminated interpolated string", ts_opening };
}

/**
 * @brief Scans a raw string literal from the source code.
 * Handles the raw string delimiter and the string content.
 *
 * @return void Always returns void; adds the raw string token to the list of
 * tokens.
 */
void Lexer::rawString () {
	const TokenSpan ts_opening = span();
	while (!isAtEnd()) {
		if (startsWith(rawStringDelimiter())) {
			advanceBytes(rawStringDelimiter().size());
			addToken(TokenType::RawStringLiteral, span(), sourceText().substr(st_start, st_current - st_start));
			return;
		}

		advanceStringScalar();
	}

	throw cde::SyntaxException{ DiagnosticCode::UnterminatedRawString, "Unterminated raw string", ts_opening };
}

/**
 * @brief Advances the lexer by one UTF-8 scalar value.
 *
 * Handles multi-byte UTF-8 sequences and validates them.
 *
 * @return void Always returns void; advances the lexer by one UTF-8 scalar
 * value.
 */
void Lexer::advanceStringScalar () {
	const std::size_t st_length = utf8ScalarLength(std::string_view{ sourceText() }.substr(st_current));
	if (st_length == 0) {
		const Position p_byte = p_current;
		advance();
		throw cde::SyntaxException{ DiagnosticCode::InvalidUnicode, "Invalid UTF-8 in literal", spanFrom(p_byte) };
	}
	advanceBytes(st_length);
}

void Lexer::advanceEscape (const Position start, std::string_view kind) {
	if (isValidEscape(peek())) {
		advance();
		return;
	}
	if (!consumeLineBreak()) advanceStringScalar();
	throw cde::SyntaxException{ DiagnosticCode::InvalidEscape, std::format("Invalid escape in {}", kind), spanFrom(start) };
}

void Lexer::advanceStringCharacter (std::string_view kind) {
	const Position p_startChar = p_current;
	const bool     b_escape    = peek() == '\\';
	if (b_escape) {
		advance();
		if (isAtEnd()) return;
	}
	const Position p_newline = p_current;
	if (consumeLineBreak()) {
		throw cde::SyntaxException{ DiagnosticCode::MultilineString, std::format("Newline in {}", kind), spanFrom(p_newline) };
	}
	if (b_escape)
		advanceEscape(p_startChar, kind);
	else
		advanceStringScalar();
}

/**
 * @brief Scans a string literal from the source code.
 *
 * Handles escape sequences and multi-byte UTF-8 sequences.
 *
 * @return void Always returns void; adds the string token to the list of
 * tokens.
 */
void Lexer::string () {
	const TokenSpan ts_opening = span();
	while (!isAtEnd()) {
		if (startsWith(S_STRING_QUOTE)) {
			advanceBytes(S_STRING_QUOTE.size());
			addToken(TokenType::StringLiteral, span(), sourceText().substr(st_start, st_current - st_start));
			return;
		}

		advanceStringCharacter("string literal");
	}

	throw cde::SyntaxException{ DiagnosticCode::UnterminatedString, "Unterminated string literal", ts_opening };
}

/**
 * @brief Scans a character literal from the source code.
 *
 * Handles escape sequences and validates that the literal contains exactly one
 * character.
 *
 * @return void Always returns void; adds the character token to the list of
 * tokens.
 */
void Lexer::character () {
	const TokenSpan ts_opening = span();
	if (isAtEnd() || isLineBreak(peek())) {
		throw cde::SyntaxException{ DiagnosticCode::UnterminatedCharacter, "Unterminated character literal", ts_opening };
	}

	if (startsWith(S_CHARACTER_QUOTE)) {
		advanceBytes(S_CHARACTER_QUOTE.size());
		throw cde::SyntaxException{ DiagnosticCode::EmptyCharacter, "Empty character literal", span() };
	}

	if (peek() == '\\') {
		const Position p_escapeStart = p_current;
		advance();
		if (isAtEnd()) {
			throw cde::SyntaxException{ DiagnosticCode::UnterminatedCharacter, "Unterminated character literal", ts_opening };
		}
		advanceEscape(p_escapeStart, "character literal");
	} else {
		advanceStringScalar();
	}

	if (!startsWith(S_CHARACTER_QUOTE)) {
		throw cde::SyntaxException{ DiagnosticCode::InvalidCharacterLength, "Character literal must contain exactly one character", span() };
	}

	advanceBytes(S_CHARACTER_QUOTE.size());
	addToken(TokenType::CharacterLiteral, span(), sourceText().substr(st_start, st_current - st_start));
}

}
