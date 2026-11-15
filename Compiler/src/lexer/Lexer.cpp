// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <lexer/Lexer.hpp>
#include <util/utf8.hpp>

namespace cloth {
namespace {

/**
 * @brief Represents the prefix used for string interpolation.
 * @param INTERPOLATION_PREFIX The prefix used for string interpolation.
 */
const std::string S_INTERPOLATION_PREFIX = std::format("{}{}", to_string(o_POUND), to_string(o_DOUBLE_QUOTATION));

/**
 * @brief Represents the prefix used for string interpolation.
 * @param INTERPOLATION_PREFIX The prefix used for string interpolation.
 */
const std::string S_RAW_STRING_DELIMITER = std::string(3, to_string(o_DOUBLE_QUOTATION).front());

/**
 * @brief Represents an entry for an operator, including its type and spelling.
 *
 * @param op The operator type.
 * @param spelling The spelling of the operator.
 * @return OperatorEntry The operator entry containing the type and spelling.
 */
struct OperatorEntry {
	Operator op;
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

	for (int value = o_PLUS; value < o_ERROR; ++value) {
		const auto op = static_cast<Operator>(value);
		v_entries.push_back({ op, to_string(op) });
	}

	std::sort(v_entries.begin(), v_entries.end(),
		  [](const OperatorEntry &left, const OperatorEntry &right) { return left.spelling.size() > right.spelling.size(); });

	return v_entries;
}();

/**
 * @brief Returns a list of all keywords except for 'auto'.
 *
 * @return std::vector<Keyword> The list of keywords excluding 'auto'.
 */
const std::vector<Keyword> V_KEYWORDS = [] {
	std::vector<Keyword> v_entries;

	for (int value = k_MOD; value < k_ERROR; ++value) {
		const auto keyword = static_cast<Keyword>(value);
		if (keyword != k_AUTO) {
			v_entries.push_back(keyword);
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
char charAt(std::string_view source, std::size_t index) {
	return index < source.size() ? source[index] : '\0';
}

/**
 * @brief Checks if the given character is a digit.
 *
 * @param c The character to check.
 * @return true If the character is a digit.
 * @return false Otherwise.
 */
bool isDigit(char c) {
	return c >= '0' && c <= '9';
}

/**
 * @brief Checks if the given character is a valid identifier start character.
 *
 * @param c The character to check.
 * @return true If the character is a valid identifier start character.
 * @return false Otherwise.
 */
bool isIdentifierStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/**
 * @brief Checks if the given character is a valid identifier part character.
 *
 * @param c The character to check.
 * @return true If the character is a valid identifier part character.
 * @return false Otherwise.
 */
bool isIdentifierPart(char c) {
	return isIdentifierStart(c) || isDigit(c);
}

/**
 * @brief Checks if the given character is a line break character.
 *
 * @param c The character to check.
 * @return true If the character is a line break character.
 * @return false Otherwise.
 */
bool isLineBreak(char c) {
	return c == '\n' || c == '\r';
}

/**
 * @brief Checks if the given character is a valid escape sequence character.
 *
 * @param c The character to check.
 * @return true If the character is a valid escape sequence character.
 * @return false Otherwise.
 */
bool isValidEscape(char c) {
	return std::string_view{ "nrt0\\'\"" }.find(c) != std::string_view::npos;
}

/**
 * @brief Reports a lexer error at the given position with the specified
 * message.
 *
 * @param position The position in the source where the error occurred.
 * @param message The error message describing the lexer error.
 */
[[noreturn]] void lexerError(Position position, std::string_view message) {
	throw std::runtime_error(std::format("{}:{}: {}", position.line, position.column, message));
}

} // namespace

/**
 * @brief Advances the lexer by one character and updates the current position.
 *
 * @return char The character that was advanced past.
 */
char Lexer::advance() {
	const char c = s_source[i_current++];
	if (c == '\n') {
		p_current = { p_current.line + 1, 1 };
	} else {
		++p_current.column;
	}
	return c;
}

/**
 * @brief Returns the current character without advancing the lexer.
 *
 * @return char The current character without advancing the lexer.
 */
char Lexer::peek() const {
	return charAt(s_source, i_current);
}

/**
 * @brief Checks if the lexer has reached the end of the source.
 *
 * @return true If the lexer is at the end of the source.
 * @return false Otherwise.
 */
bool Lexer::isAtEnd() const {
	return static_cast<std::size_t>(i_current) >= s_source.size();
}

/**
 * @brief Scans the source and returns a vector of tokens.
 *
 * @return std::vector<Token> The scanned tokens.
 */
std::vector<Token> Lexer::scanTokens() {
	scan();
	return v_tokens;
}

void Lexer::scan() {
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
void Lexer::addToken(TokenType type, TokenSpan span, const std::string &lexeme) {
	v_tokens.push_back(Token(lexeme, type, span));
}

/**
 * @brief Returns the span of the current token.
 *
 * @return TokenSpan The span of the current token.
 */
TokenSpan Lexer::span() {
	return { p_start.line, p_start.column, p_current.line, p_current.column };
}

/**
 * @brief Returns the span at the current cursor position.
 *
 * @return TokenSpan The span at the current cursor position.
 */
TokenSpan Lexer::cursorSpan() {
	return { p_current.line, p_current.column, p_current.line, p_current.column };
}

/**
 * @brief Matches the next operator in the source.
 *
 * @return Operator The matched operator, or o_ERROR if no
 * operator matches.
 */
Operator Lexer::matchOperator() {
	const std::string_view s_remaining = std::string_view{ s_source }.substr(i_current);

	for (const auto &[op, spelling] : V_OPERATORS) {
		if (!s_remaining.starts_with(spelling)) {
			continue;
		}

		if (op == o_UNDERSCORE && isIdentifierPart(charAt(s_remaining, spelling.size()))) {
			continue;
		}

		for (std::size_t i = 0; i < spelling.size(); ++i) {
			advance();
		}
		return op;
	}

	return o_ERROR;
}

/**
 * @brief Scans the next token from the source and adds it to the list of
 * tokens.
 * @return void
 */
void Lexer::scanToken() {
	const std::string_view s_remaining = std::string_view{ s_source }.substr(i_current);

	if (s_remaining.starts_with(S_INTERPOLATION_PREFIX)) {
		for (std::size_t i = 0; i < S_INTERPOLATION_PREFIX.size(); ++i) {
			advance();
		}
		addToken(TokenType::InterpolationStart, span(), s_source.substr(i_start, i_current - i_start));
		interpolatedString();
		return;
	}

	if (s_remaining.starts_with(S_RAW_STRING_DELIMITER)) {
		for (std::size_t i = 0; i < S_RAW_STRING_DELIMITER.size(); ++i) {
			advance();
		}
		rawString();
		return;
	}

	if (peek() == '"') {
		advance();
		string();
		return;
	}

	if (peek() == '\'') {
		advance();
		character();
		return;
	}

	if (skipComments()) {
		return;
	}

	if (const auto op = matchOperator(); op != o_ERROR) {
		v_tokens.emplace_back(s_source.substr(i_start, i_current - i_start), op, span());
		return;
	}

	const char c_character = advance();
	switch (c_character) {
	case ' ':
	case '\r':
	case '\t':
		return;
	case '\n':
		addToken(TokenType::NewLine, span(), "\n");
		return;
	default:
		if (isDigit(c_character)) {
			number();
		} else if (isIdentifierStart(c_character)) {
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
bool Lexer::skipComments() {
	if (peek() != '/') {
		return false;
	}

	switch (charAt(s_source, i_current + 1)) {
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
			if (peek() == '*' && charAt(s_source, i_current + 1) == '/') {
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
void Lexer::number() {
	bool b_isFloat = false;
	int i_radix = 10;
	if (s_source[i_start] == '0') {
		switch (peek()) {
		case 'x':
			i_radix = 16;
			break;
		case 'b':
			i_radix = 2;
			break;
		case 'o':
			i_radix = 8;
			break;
		}
	}

	const auto digit = [i_radix](char c) {
		if (isDigit(c)) {
			return c - '0' < i_radix;
		}
		return i_radix == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
	};

	const auto completeSuffix = [this](char lower, char upper) {
		const char suffix = charAt(s_source, i_current + 1);
		return (suffix == lower || suffix == upper) && !isIdentifierPart(charAt(s_source, i_current + 2));
	};

	const auto invalid = [this](std::string_view reason) {
		while (isIdentifierPart(peek())) {
			advance();
		}
		lexerError(p_start, std::format("Invalid numeric literal '{}': {}", s_source.substr(i_start, i_current - i_start), reason));
	};

	const auto consumeDigits = [this, &digit] {
		while (digit(peek()) || peek() == '_') {
			if (peek() == '_' && (!digit(s_source[i_current - 1]) || !digit(charAt(s_source, i_current + 1)))) {
				lexerError(p_current, "Numeric separators must appear between digits");
			}
			advance();
		}
	};

	if (i_radix != 10) {
		advance();
		if (peek() == '_') {
			lexerError(p_current, "Numeric separators must appear between digits");
		}
		if (!digit(peek())) {
			invalid(std::format("Expected a base-{} digit after the prefix", i_radix));
		}
	}
	consumeDigits();

	if (peek() == '.') {
		const char next = charAt(s_source, i_current + 1);
		const bool trailingFloat = completeSuffix('f', 'F');
		const bool trailingInteger = completeSuffix('i', 'I');
		if (isDigit(next) || trailingFloat || trailingInteger || next == '_') {
			advance();
			if (i_radix != 10) {
				invalid("Base-prefixed literals cannot have a decimal point or suffix");
			}
			if (trailingInteger) {
				invalid("An integer suffix cannot follow a decimal point");
			}
			if (peek() == '_') {
				lexerError(p_current, "Numeric separators must appear between digits");
			}
			b_isFloat = true;
			consumeDigits();
		}
	}

	const char suffix = peek();
	if (i_radix == 10 && (suffix == 'f' || suffix == 'F' || suffix == 'i' || suffix == 'I')) {
		advance();
		const bool floatSuffix = suffix == 'f' || suffix == 'F';
		if (b_isFloat && !floatSuffix) {
			invalid("An integer suffix cannot follow a decimal point");
		}
		b_isFloat = b_isFloat || floatSuffix;
	}

	if (isIdentifierPart(peek())) {
		if (i_radix == 10) {
			invalid("Invalid numeric suffix");
		} else {
			invalid(std::format("Invalid base-{} digit or forbidden suffix", i_radix));
		}
	}

	addToken(b_isFloat ? TokenType::FloatingPointLiteral : TokenType::IntegerLiteral, span(), s_source.substr(i_start, i_current - i_start));
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
void Lexer::identifier() {
	while (isIdentifierPart(peek())) {
		advance();
	}

	const std::string s_text = s_source.substr(i_start, i_current - i_start);
	const bool b_isKeyword = std::any_of(V_KEYWORDS.begin(), V_KEYWORDS.end(), [&s_text](Keyword keyword) { return to_string(keyword) == s_text; });

	addToken(b_isKeyword ? TokenType::Keyword : TokenType::Identifier, span(), s_text);
}

/**
 * @brief Scans an interpolated string from the source code.
 * Handles string segments, interpolation holes, and escape sequences.
 *
 * @return void Always returns void; adds the interpolated string tokens to the
 * list of tokens.
 */
void Lexer::interpolatedString() {
	const Position p_opening = p_start;
	const auto mark = [this] {
		i_start = i_current;
		p_start = p_current;
	};
	const auto emit = [this](TokenType type) { addToken(type, span(), s_source.substr(i_start, i_current - i_start)); };

	mark();
	while (!isAtEnd()) {
		const char c_character = peek();
		if (c_character == '"') {
			emit(TokenType::StringSegment);
			mark();
			advance();
			emit(TokenType::InterpolationEnd);
			return;
		}

		if (c_character == '{' || c_character == '}') {
			if (charAt(s_source, i_current + 1) == c_character) {
				advance();
				advance();
				continue;
			}
			if (c_character == '}') {
				lexerError(p_current, "Expected '}}' for a literal closing brace");
			}

			emit(TokenType::StringSegment);
			mark();
			advance();
			emit(TokenType::InterpolationOpen);
			const auto hole = p_start;
			int depth = 0;
			while (!isAtEnd()) {
				mark();
				if (peek() == '}' && depth == 0) {
					break;
				}
				if (peek() == '{') {
					++depth;
				} else if (peek() == '}') {
					--depth;
				}
				scanToken();
			}
			if (isAtEnd()) {
				lexerError(hole, "Unterminated interpolation");
			}

			advance();
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
			const char escaped = advance();
			if (!isValidEscape(escaped)) {
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
void Lexer::rawString() {
	while (!isAtEnd()) {
		if (std::string_view{ s_source }.substr(i_current).starts_with(S_RAW_STRING_DELIMITER)) {
			for (std::size_t i = 0; i < S_RAW_STRING_DELIMITER.size(); ++i) {
				advance();
			}
			addToken(TokenType::RawStringLiteral, span(), s_source.substr(i_start, i_current - i_start));
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
void Lexer::advanceStringScalar() {
	const auto length = utf8_scalar_length(std::string_view{ s_source }.substr(i_current));
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
void Lexer::string() {
	while (!isAtEnd()) {
		const char c_character = peek();
		if (c_character == '"') {
			advance();
			addToken(TokenType::StringLiteral, span(), s_source.substr(i_start, i_current - i_start));
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
			const char c_escaped = advance();
			if (!isValidEscape(c_escaped)) {
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
void Lexer::character() {
	if (isAtEnd() || isLineBreak(peek())) {
		lexerError(p_start, "Unterminated character literal");
	}

	if (peek() == '\'') {
		lexerError(p_start, "Empty character literal");
	}

	if (peek() == '\\') {
		const Position p_escapeStart = p_current;
		advance();
		if (isAtEnd()) {
			lexerError(p_start, "Unterminated character literal");
		}
		const char escaped = advance();
		if (!isValidEscape(escaped)) {
			lexerError(p_escapeStart, "Invalid escape in character literal");
		}
	} else {
		advanceStringScalar();
	}

	if (peek() != '\'') {
		lexerError(p_start, "Character literal must contain exactly one character");
	}

	advance();
	addToken(TokenType::CharacterLiteral, span(), s_source.substr(i_start, i_current - i_start));
}

}
