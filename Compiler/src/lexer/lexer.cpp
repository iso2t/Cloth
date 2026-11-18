// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <cde/syntax_exception.hpp>
#include <lexer/lexer.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>
#include <util/utf8.hpp>

namespace cloth {
namespace {

const std::string_view S_STRING_QUOTE         = to_string(oDoubleQuotation);
const std::string_view S_CHARACTER_QUOTE      = to_string(oQuotation);
const std::string_view S_INTERPOLATION_OPEN   = to_string(oBraceOpen);
const std::string_view S_INTERPOLATION_CLOSE  = to_string(oBraceClose);
const std::string      S_INTERPOLATION_PREFIX = std::format("{}{}", to_string(oPound), S_STRING_QUOTE);

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

	for (int i_value = oPlus; i_value < oError; ++i_value) {
		if (const auto o_operator = static_cast<Operator>(i_value); isTokenized(o_operator)) {
			v_entries.push_back({ .op = o_operator, .spelling = to_string(o_operator) });
		}
	}

	std::ranges::sort(v_entries, [] (const OperatorEntry &left, const OperatorEntry &right) { return left.spelling.size() > right.spelling.size(); });

	return v_entries;
}();

// Contextual keywords keep their identifier token outside the grammar that recognizes them.
const std::vector<Keyword> V_KEYWORDS = [] {
	std::vector<Keyword> v_entries;

	for (int i_value = kMod; i_value < kError; ++i_value) {
		if (const Keyword k_keyword = static_cast<Keyword>(i_value); isReserved(k_keyword)) {
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

/**
 * @brief Checks if the given character is a valid identifier start character.
 *
 * @param c The character to check.
 * @return true If the character is a valid identifier start character.
 * @return false Otherwise.
 */
bool isIdentifierStart (const char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

std::size_t identifierPartLength (const std::string_view source, const std::size_t offset) {
	if (offset >= source.size()) {
		return 0;
	}
	return isIdentifierStart(source[offset]) ? 1 : decimalDigit(source.substr(offset)).length;
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

/**
 * @brief Reports a lexer error at the given position with the specified
 * message.
 *
 * @param position The position in the source where the error occurred.
 * @param message The error message describing the lexer error.
 */
[[noreturn]] void lexerError (Position position, std::string_view message) {
	throw std::runtime_error(std::format("{}:{}: {}", position.line, position.column, message));
}

} // namespace

/**
 * @brief Advances the lexer by one character and updates the current position.
 *
 * @return char The character that was advanced past.
 */
char Lexer::advance () {
	const char c = source_text()[i_current++];
	if (c == '\n') {
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

/**
 * @brief Returns the current character without advancing the lexer.
 *
 * @return char The current character without advancing the lexer.
 */
char Lexer::peek () const {
	return charAt(source_text(), i_current);
}

bool Lexer::startsWith (std::string_view spelling) const {
	return std::string_view(source_text()).substr(i_current).starts_with(spelling);
}

/**
 * @brief Checks if the lexer has reached the end of the source.
 *
 * @return true If the lexer is at the end of the source.
 * @return false Otherwise.
 */
bool Lexer::isAtEnd () const {
	return static_cast<std::size_t>(i_current) >= source_text().size();
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
		i_start = i_current;
		p_start = p_current;
		scanToken();
	}

	if (v_tokens.empty() || v_tokens.back().get_type() != TokenType::EndOfFile) {
		addToken(TokenType::EndOfFile, cursorSpan(), "");
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
	return { .startLine = p_start.line, .startCol = p_start.column, .endLine = p_current.line, .endCol = p_current.column, .source = p_source };
}

/**
 * @brief Returns the span at the current cursor position.
 *
 * @return TokenSpan The span at the current cursor position.
 */
TokenSpan Lexer::cursorSpan () const {
	return { .startLine = p_current.line, .startCol = p_current.column, .endLine = p_current.line, .endCol = p_current.column, .source = p_source };
}

/**
 * @brief Matches the next operator in the source.
 *
 * @return Operator The matched operator, or o_ERROR if no
 * operator matches.
 */
Operator Lexer::matchOperator () {
	const std::string_view s_remaining = std::string_view{ source_text() }.substr(i_current);

	for (const auto &[op, spelling] : V_OPERATORS) {
		if (!s_remaining.starts_with(spelling)) {
			continue;
		}

		if (op == oUnderscore && identifierPartLength(s_remaining, spelling.size()) != 0) {
			continue;
		}

		for (std::size_t i = 0; i < spelling.size(); ++i) {
			advance();
		}
		return op;
	}

	return oError;
}

/**
 * @brief Scans the next token from the source and adds it to the list of
 * tokens.
 * @return void
 */
void Lexer::scanToken () {
	const std::string_view s_remaining = std::string_view{ source_text() }.substr(i_current);

	if (s_remaining.starts_with(S_INTERPOLATION_PREFIX)) {
		advanceBytes(S_INTERPOLATION_PREFIX.size());
		addToken(TokenType::InterpolationStart, span(), source_text().substr(i_start, i_current - i_start));
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

	if (const auto op = matchOperator(); op != oError) {
		v_tokens.emplace_back(source_text().substr(i_start, i_current - i_start), op, span());
		return;
	}

	if (decimalDigit(s_remaining).length != 0) {
		number();
		return;
	}

	switch (const char c_character = advance()) {
	case ' ':
	case '\r':
	case '\t':
		return;
	case '\n':
		addToken(TokenType::NewLine, span(), "\n");
		return;
	default:
		if (isIdentifierStart(c_character)) {
			identifier();
		} else {
			lexerError(p_start, std::format("Unexpected byte 0x{:02X}", static_cast<unsigned char>(c_character)));
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

	switch (charAt(source_text(), i_current + 1)) {
	case '/':
		advance();
		advance();
		while (!isAtEnd() && peek() != '\n') {
			advance();
		}
		return true;

	case '*':
		advance();
		advance();
		while (!isAtEnd()) {
			if (peek() == '*' && charAt(source_text(), i_current + 1) == '/') {
				advance();
				advance();
				return true;
			}
			advance();
		}
		lexerError(p_start, "Unterminated block comment");

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
	const std::string_view s_source  = source_text();
	const int              i_radix   = integerLiteralRadix(s_source.substr(i_current));
	bool                   b_isFloat = false;

	if (i_radix != 10) {
		advanceBytes(2);
		if (peek() == '_') {
			invalidNumericSeparator();
		}
		if (radixDigitLength(s_source, i_current, i_radix) == 0) {
			invalidNumber(std::format("Expected a base-{} digit after the prefix", i_radix));
		}
	}
	consumeNumericDigits(i_radix);

	if (peek() == '.') {
		const char c_next           = charAt(s_source, i_current + 1);
		const bool b_completeSuffix = identifierPartLength(s_source, i_current + 2) == 0;
		const bool b_trailingFloat  = b_completeSuffix && (c_next == 'f' || c_next == 'F');
		if (const bool b_trailingInteger = b_completeSuffix && (c_next == 'i' || c_next == 'I');
		    decimalDigit(s_source.substr(i_current + 1)).length != 0 || b_trailingFloat || b_trailingInteger || c_next == '_') {
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

	if (identifierPartLength(s_source, i_current) != 0) {
		invalidNumber(i_radix == 10 ? "Invalid numeric suffix" : std::format("Invalid base-{} digit or forbidden suffix", i_radix));
	}

	addToken(b_isFloat ? TokenType::FloatingPointLiteral : TokenType::IntegerLiteral, span(), source_text().substr(i_start, i_current - i_start));
}

void Lexer::consumeNumericDigits (const int radix) {
	const std::string_view s_source = source_text();
	for (std::size_t st_length = radixDigitLength(s_source, i_current, radix); st_length != 0; st_length = radixDigitLength(s_source, i_current, radix)) {
		advanceBytes(st_length);
		if (peek() == '_') {
			if (radixDigitLength(s_source, i_current + 1, radix) == 0) {
				invalidNumericSeparator();
			}
			advance();
		}
	}
}

void Lexer::invalidNumber (std::string_view reason) {
	consumeIdentifierParts();
	throw cde::SyntaxException{ cde::DiagnosticCode::INVALID_NUMERIC_LITERAL,
		                    std::format("Invalid numeric literal '{}': {}", source_text().substr(i_start, i_current - i_start), reason), span() };
}

void Lexer::invalidNumericSeparator () const {
	TokenSpan ts_separator = cursorSpan();
	++ts_separator.endCol;
	throw cde::SyntaxException{ cde::DiagnosticCode::INVALID_SEPARATOR, "Numeric separators must appear between digits", ts_separator };
}

void Lexer::consumeIdentifierParts () {
	const std::string_view s_source = source_text();
	for (std::size_t st_length = identifierPartLength(s_source, i_current); st_length != 0; st_length = identifierPartLength(s_source, i_current)) {
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

	const std::string s_text      = source_text().substr(i_start, i_current - i_start);
	const bool        b_isKeyword = std::ranges::any_of(V_KEYWORDS, [&s_text] (const Keyword keyword) { return to_string(keyword) == s_text; });

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
	const Position p_opening = p_start;
	const auto     mark      = [this] {
                i_start = i_current;
                p_start = p_current;
	};
	const auto emit = [this] (const TokenType type) { addToken(type, span(), source_text().substr(i_start, i_current - i_start)); };

	mark();
	while (!isAtEnd()) {
		const char c_character = peek();
		if (startsWith(S_STRING_QUOTE)) {
			emit(TokenType::StringSegment);
			mark();
			advanceBytes(S_STRING_QUOTE.size());
			emit(TokenType::InterpolationEnd);
			return;
		}

		const bool b_open = startsWith(S_INTERPOLATION_OPEN);
		if (b_open || startsWith(S_INTERPOLATION_CLOSE)) {
			const std::string_view s_brace = b_open ? S_INTERPOLATION_OPEN : S_INTERPOLATION_CLOSE;
			if (std::string_view(source_text()).substr(i_current + s_brace.size()).starts_with(s_brace)) {
				advanceBytes(2 * s_brace.size());
				continue;
			}
			if (!b_open) {
				lexerError(p_current, std::format("Expected '{0}{0}' for a literal closing brace", s_brace));
			}

			emit(TokenType::StringSegment);
			mark();
			advanceBytes(S_INTERPOLATION_OPEN.size());
			emit(TokenType::InterpolationOpen);
			const auto hole  = p_start;
			int        depth = 0;
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
				lexerError(hole, "Unterminated interpolation");
			}

			advanceBytes(S_INTERPOLATION_CLOSE.size());
			emit(TokenType::InterpolationClose);
			mark();
			continue;
		}

		if (c_character == '\\') {
			const auto escapeStart = p_current;
			advance();
			if (isAtEnd()) {
				break;
			}
			if (isLineBreak(peek())) {
				lexerError(p_current, "Newline in interpolated string");
			}
			if (const char escaped = advance(); !isValidEscape(escaped)) {
				lexerError(escapeStart, "Invalid escape in interpolated string");
			}
		} else if (isLineBreak(c_character)) {
			lexerError(p_current, "Newline in interpolated string");
		} else {
			advanceStringScalar();
		}
	}

	lexerError(p_opening, "Unterminated interpolated string");
}

/**
 * @brief Scans a raw string literal from the source code.
 * Handles the raw string delimiter and the string content.
 *
 * @return void Always returns void; adds the raw string token to the list of
 * tokens.
 */
void Lexer::rawString () {
	while (!isAtEnd()) {
		if (startsWith(rawStringDelimiter())) {
			advanceBytes(rawStringDelimiter().size());
			addToken(TokenType::RawStringLiteral, span(), source_text().substr(i_start, i_current - i_start));
			return;
		}

		advanceStringScalar();
	}

	lexerError(p_start, "Unterminated raw string");
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
	const auto length = utf8_scalar_length(std::string_view{ source_text() }.substr(i_current));
	if (length == 0) {
		lexerError(p_current, "Invalid UTF-8 in literal");
	}
	for (std::size_t i = 0; i < length; ++i) {
		advance();
	}
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
	while (!isAtEnd()) {
		const char c_character = peek();
		if (startsWith(S_STRING_QUOTE)) {
			advanceBytes(S_STRING_QUOTE.size());
			addToken(TokenType::StringLiteral, span(), source_text().substr(i_start, i_current - i_start));
			return;
		}

		if (c_character == '\\') {
			const Position p_escapeStart = p_current;
			advance();
			if (isAtEnd()) {
				break;
			}
			if (isLineBreak(peek())) {
				lexerError(p_current, "Newline in string literal");
			}
			if (const char c_escaped = advance(); !isValidEscape(c_escaped)) {
				lexerError(p_escapeStart, "Invalid escape in string literal");
			}
		} else if (isLineBreak(c_character)) {
			lexerError(p_current, "Newline in string literal");
		} else {
			advanceStringScalar();
		}
	}

	lexerError(p_start, "Unterminated string literal");
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
	if (isAtEnd() || isLineBreak(peek())) {
		lexerError(p_start, "Unterminated character literal");
	}

	if (startsWith(S_CHARACTER_QUOTE)) {
		lexerError(p_start, "Empty character literal");
	}

	if (peek() == '\\') {
		const Position p_escapeStart = p_current;
		advance();
		if (isAtEnd()) {
			lexerError(p_start, "Unterminated character literal");
		}
		if (const char escaped = advance(); !isValidEscape(escaped)) {
			lexerError(p_escapeStart, "Invalid escape in character literal");
		}
	} else {
		advanceStringScalar();
	}

	if (!startsWith(S_CHARACTER_QUOTE)) {
		lexerError(p_start, "Character literal must contain exactly one character");
	}

	advanceBytes(S_CHARACTER_QUOTE.size());
	addToken(TokenType::CharacterLiteral, span(), source_text().substr(i_start, i_current - i_start));
}

}
