#include "../../include/lexer/Lexer.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

/**
 * @brief Represents the prefix used for string interpolation.
 * @param INTERPOLATION_PREFIX The prefix used for string interpolation.
 */
const auto INTERPOLATION_PREFIX =
    std::format("{}{}", Tokens::to_string(Tokens::o_POUND),
                Tokens::to_string(Tokens::o_DOUBLE_QUOTATION));

/**
 * @brief Represents the prefix used for string interpolation.
 * @param INTERPOLATION_PREFIX The prefix used for string interpolation.
 */
const auto RAW_STRING_DELIMITER =
    std::string(3, Tokens::to_string(Tokens::o_DOUBLE_QUOTATION).front());

/**
 * @brief Represents an entry for an operator, including its type and spelling.
 *
 * @param op The operator type.
 * @param spelling The spelling of the operator.
 * @return OperatorEntry The operator entry containing the type and spelling.
 */
struct OperatorEntry {
  Tokens::Operator op;
  std::string_view spelling;
};

/**
 * @brief Returns a list of all operators sorted by descending spelling length.
 *
 * @return std::vector<OperatorEntry> The list of operators sorted by descending
 * spelling length.
 */
const auto OPERATORS = [] {
  std::vector<OperatorEntry> entries;

  for (int value = Tokens::o_PLUS; value < Tokens::o_ERROR; ++value) {
    const auto op = static_cast<Tokens::Operator>(value);
    entries.push_back({op, Tokens::to_string(op)});
  }

  std::sort(entries.begin(), entries.end(),
            [](const OperatorEntry &left, const OperatorEntry &right) {
              return left.spelling.size() > right.spelling.size();
            });

  return entries;
}();

/**
 * @brief Returns a list of all keywords except for 'auto'.
 *
 * @return std::vector<Tokens::Keyword> The list of keywords excluding 'auto'.
 */
const auto KEYWORDS = [] {
  std::vector<Tokens::Keyword> entries;

  for (int value = Tokens::k_MOD; value < Tokens::k_ERROR; ++value) {
    const auto keyword = static_cast<Tokens::Keyword>(value);
    if (keyword != Tokens::k_AUTO) {
      entries.push_back(keyword);
    }
  }

  return entries;
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
bool isDigit(char c) { return c >= '0' && c <= '9'; }

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
bool isIdentifierPart(char c) { return isIdentifierStart(c) || isDigit(c); }

/**
 * @brief Checks if the given character is a line break character.
 *
 * @param c The character to check.
 * @return true If the character is a line break character.
 * @return false Otherwise.
 */
bool isLineBreak(char c) { return c == '\n' || c == '\r'; }

/**
 * @brief Checks if the given character is a valid escape sequence character.
 *
 * @param c The character to check.
 * @return true If the character is a valid escape sequence character.
 * @return false Otherwise.
 */
bool isValidEscape(char c) {
  return std::string_view{"nrt0\\'\""}.find(c) != std::string_view::npos;
}

/**
 * @brief Reports a lexer error at the given position with the specified
 * message.
 *
 * @param position The position in the source where the error occurred.
 * @param message The error message describing the lexer error.
 */
[[noreturn]] void lexerError(Position position, std::string_view message) {
  throw std::runtime_error(
      std::format("{}:{}: {}", position.line, position.column, message));
}

std::string quoteLexeme(std::string_view lexeme) {
  std::string quoted(1, '"');
  quoted.reserve(lexeme.size() + 2);
  for (const char c : lexeme) {
    switch (c) {
    case '"':
    case '\\':
      quoted += '\\';
      quoted += c;
      break;
    case '\n':
      quoted += "\\n";
      break;
    case '\r':
      quoted += "\\r";
      break;
    case '\t':
      quoted += "\\t";
      break;
    default:
      const auto byte = static_cast<unsigned char>(c);
      if (byte < 0x20 || byte >= 0x7F) {
        quoted += std::format("\\x{:02X}", byte);
      } else {
        quoted += c;
      }
    }
  }
  quoted += '"';
  return quoted;
}

} // namespace

/**
 * @brief Advances the lexer by one character and updates the current position.
 *
 * @return char The character that was advanced past.
 */
char Lexer::advance() {
  const char c = source[current++];
  if (c == '\n') {
    p_current = {p_current.line + 1, 1};
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
char Lexer::peek() const { return charAt(source, current); }

/**
 * @brief Checks if the lexer has reached the end of the source.
 *
 * @return true If the lexer is at the end of the source.
 * @return false Otherwise.
 */
bool Lexer::isAtEnd() const {
  return static_cast<std::size_t>(current) >= source.size();
}

/**
 * @brief Scans the source and returns a vector of tokens.
 *
 * @return std::vector<Token> The scanned tokens.
 */
std::vector<Token> Lexer::scanTokens() {
  scan();
  return tokens;
}

/**
 * @brief Scans the source and writes a table of its tokens.
 */
void Lexer::printTokens(std::ostream &out) {
  scan();

  constexpr std::string_view lexemeHeader = "LEXEME";
  constexpr std::string_view typeHeader = "TYPE";
  constexpr std::string_view spanHeader = "SPAN";
  auto lexemeWidth = lexemeHeader.size();
  auto typeWidth = typeHeader.size();
  auto spanWidth = spanHeader.size();

  struct Row {
    std::string lexeme;
    std::string_view type;
    std::string span;
  };
  std::vector<Row> rows;
  rows.reserve(tokens.size());
  for (const auto &token : tokens) {
    const auto &row =
        rows.emplace_back(quoteLexeme(token.get_lexeme()), token.type_name(),
                          token.get_span().to_string());
    lexemeWidth = std::max(lexemeWidth, row.lexeme.size());
    typeWidth = std::max(typeWidth, row.type.size());
    spanWidth = std::max(spanWidth, row.span.size());
  }

  const auto printRow = [&](std::string_view lexeme, std::string_view type,
                            std::string_view spanText) {
    out << std::format("{:<{}}  {:<{}}  {}\n", lexeme, lexemeWidth, type,
                       typeWidth, spanText);
  };
  printRow(lexemeHeader, typeHeader, spanHeader);
  printRow(std::string(lexemeWidth, '-'), std::string(typeWidth, '-'),
           std::string(spanWidth, '-'));
  for (const auto &row : rows) {
    printRow(row.lexeme, row.type, row.span);
  }

  out << "Count: " << tokens.size();
}

void Lexer::scan() {
  while (!isAtEnd()) {
    start = current;
    p_start = p_current;
    scanToken();
  }

  if (tokens.empty() || tokens.back().get_type() != TokenType::EndOfFile) {
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
void Lexer::addToken(TokenType type, TokenSpan span,
                     const std::string &lexeme) {
  tokens.push_back(Token(lexeme, type, span));
}

/**
 * @brief Returns the span of the current token.
 *
 * @return TokenSpan The span of the current token.
 */
TokenSpan Lexer::span() {
  return {p_start.line, p_start.column, p_current.line, p_current.column};
}

/**
 * @brief Returns the span at the current cursor position.
 *
 * @return TokenSpan The span at the current cursor position.
 */
TokenSpan Lexer::cursorSpan() {
  return {p_current.line, p_current.column, p_current.line, p_current.column};
}

/**
 * @brief Matches the next operator in the source.
 *
 * @return Tokens::Operator The matched operator, or Tokens::o_ERROR if no
 * operator matches.
 */
Tokens::Operator Lexer::matchOperator() {
  const auto remaining = std::string_view{source}.substr(current);

  for (const auto &[op, spelling] : OPERATORS) {
    if (!remaining.starts_with(spelling)) {
      continue;
    }

    if (op == Tokens::o_UNDERSCORE &&
        isIdentifierPart(charAt(remaining, spelling.size()))) {
      continue;
    }

    for (std::size_t i = 0; i < spelling.size(); ++i) {
      advance();
    }
    return op;
  }

  return Tokens::o_ERROR;
}

/**
 * @brief Scans the next token from the source and adds it to the list of
 * tokens.
 * @return void
 */
void Lexer::scanToken() {
  const auto remaining = std::string_view{source}.substr(current);

  if (remaining.starts_with(INTERPOLATION_PREFIX)) {
    for (std::size_t i = 0; i < INTERPOLATION_PREFIX.size(); ++i) {
      advance();
    }
    addToken(TokenType::InterpolationStart, span(),
             source.substr(start, current - start));
    interpolatedString();
    return;
  }

  if (remaining.starts_with(RAW_STRING_DELIMITER)) {
    for (std::size_t i = 0; i < RAW_STRING_DELIMITER.size(); ++i) {
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

  if (const auto op = matchOperator(); op != Tokens::o_ERROR) {
    tokens.emplace_back(source.substr(start, current - start), op, span());
    return;
  }

  const char c = advance();
  switch (c) {
  case ' ':
  case '\r':
  case '\t':
    return;
  case '\n':
    addToken(TokenType::NewLine, span(), "\n");
    return;
  default:
    if (isDigit(c)) {
      number();
    } else if (isIdentifierStart(c)) {
      identifier();
    } else {
      lexerError(p_start, std::format("Unexpected byte 0x{:02X}",
                                      static_cast<unsigned char>(c)));
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

  switch (charAt(source, current + 1)) {
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
      if (peek() == '*' && charAt(source, current + 1) == '/') {
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
  bool isFloat = false;
  int radix = 10;
  if (source[start] == '0') {
    switch (peek()) {
    case 'x':
      radix = 16;
      break;
    case 'b':
      radix = 2;
      break;
    case 'o':
      radix = 8;
      break;
    }
  }

  const auto digit = [radix](char c) {
    if (isDigit(c)) {
      return c - '0' < radix;
    }
    return radix == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
  };

  const auto completeSuffix = [this](char lower, char upper) {
    const char suffix = charAt(source, current + 1);
    return (suffix == lower || suffix == upper) &&
           !isIdentifierPart(charAt(source, current + 2));
  };

  const auto invalid = [this](std::string_view reason) {
    while (isIdentifierPart(peek())) {
      advance();
    }
    lexerError(p_start,
               std::format("Invalid numeric literal '{}': {}",
                           source.substr(start, current - start), reason));
  };

  const auto consumeDigits = [this, &digit] {
    while (digit(peek()) || peek() == '_') {
      if (peek() == '_' && (!digit(source[current - 1]) ||
                            !digit(charAt(source, current + 1)))) {
        lexerError(p_current, "Numeric separators must appear between digits");
      }
      advance();
    }
  };

  if (radix != 10) {
    advance();
    if (peek() == '_') {
      lexerError(p_current, "Numeric separators must appear between digits");
    }
    if (!digit(peek())) {
      invalid(std::format("Expected a base-{} digit after the prefix", radix));
    }
  }
  consumeDigits();

  if (peek() == '.') {
    const char next = charAt(source, current + 1);
    const bool trailingFloat = completeSuffix('f', 'F');
    const bool trailingInteger = completeSuffix('i', 'I');
    if (isDigit(next) || trailingFloat || trailingInteger || next == '_') {
      advance();
      if (radix != 10) {
        invalid("Base-prefixed literals cannot have a decimal point or suffix");
      }
      if (trailingInteger) {
        invalid("An integer suffix cannot follow a decimal point");
      }
      if (peek() == '_') {
        lexerError(p_current, "Numeric separators must appear between digits");
      }
      isFloat = true;
      consumeDigits();
    }
  }

  const char suffix = peek();
  if (radix == 10 &&
      (suffix == 'f' || suffix == 'F' || suffix == 'i' || suffix == 'I')) {
    advance();
    const bool floatSuffix = suffix == 'f' || suffix == 'F';
    if (isFloat && !floatSuffix) {
      invalid("An integer suffix cannot follow a decimal point");
    }
    isFloat = isFloat || floatSuffix;
  }

  if (isIdentifierPart(peek())) {
    if (radix == 10) {
      invalid("Invalid numeric suffix");
    } else {
      invalid(std::format("Invalid base-{} digit or forbidden suffix", radix));
    }
  }

  addToken(isFloat ? TokenType::FloatingPointLiteral
                   : TokenType::IntegerLiteral,
           span(), source.substr(start, current - start));
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

  const auto text = source.substr(start, current - start);
  const bool isKeyword = std::any_of(
      KEYWORDS.begin(), KEYWORDS.end(), [&text](Tokens::Keyword keyword) {
        return Tokens::to_string(keyword) == text;
      });

  addToken(isKeyword ? TokenType::Keyword : TokenType::Identifier, span(),
           text);
}

/**
 * @brief Scans an interpolated string from the source code.
 * Handles string segments, interpolation holes, and escape sequences.
 *
 * @return void Always returns void; adds the interpolated string tokens to the
 * list of tokens.
 */
void Lexer::interpolatedString() {
  const auto opening = p_start;
  const auto mark = [this] {
    start = current;
    p_start = p_current;
  };
  const auto emit = [this](TokenType type) {
    addToken(type, span(), source.substr(start, current - start));
  };

  mark();
  while (!isAtEnd()) {
    const char c = peek();
    if (c == '"') {
      emit(TokenType::StringSegment);
      mark();
      advance();
      emit(TokenType::InterpolationEnd);
      return;
    }

    if (c == '{' || c == '}') {
      if (charAt(source, current + 1) == c) {
        advance();
        advance();
        continue;
      }
      if (c == '}') {
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

    if (c == '\\') {
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
    } else if (isLineBreak(c)) {
      lexerError(p_current, "Newline in interpolated string");
    } else {
      advanceStringScalar();
    }
  }

  lexerError(opening, "Unterminated interpolated string");
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
    if (std::string_view{source}.substr(current).starts_with(
            RAW_STRING_DELIMITER)) {
      for (std::size_t i = 0; i < RAW_STRING_DELIMITER.size(); ++i) {
        advance();
      }
      addToken(TokenType::RawStringLiteral, span(),
               source.substr(start, current - start));
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
  const auto position = p_current;
  const auto first = static_cast<unsigned char>(advance());
  if (first < 0x80) {
    return;
  }

  // Bound the second byte to reject overlong encodings, surrogates and
  // code points beyond U+10FFFF.
  int continuationCount;
  unsigned char secondMin = 0x80;
  unsigned char secondMax = 0xBF;
  if (first >= 0xC2 && first <= 0xDF) {
    continuationCount = 1;
  } else if (first >= 0xE0 && first <= 0xEF) {
    continuationCount = 2;
    if (first == 0xE0) {
      secondMin = 0xA0;
    } else if (first == 0xED) {
      secondMax = 0x9F;
    }
  } else if (first >= 0xF0 && first <= 0xF4) {
    continuationCount = 3;
    if (first == 0xF0) {
      secondMin = 0x90;
    } else if (first == 0xF4) {
      secondMax = 0x8F;
    }
  } else {
    lexerError(position, "Invalid UTF-8 in literal");
  }

  const auto second = static_cast<unsigned char>(peek());
  if (second < secondMin || second > secondMax) {
    lexerError(position, "Invalid UTF-8 in literal");
  }
  advance();

  for (int i = 1; i < continuationCount; ++i) {
    const auto next = static_cast<unsigned char>(peek());
    if (next < 0x80 || next > 0xBF) {
      lexerError(position, "Invalid UTF-8 in literal");
    }
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
    const char c = peek();
    if (c == '"') {
      advance();
      addToken(TokenType::StringLiteral, span(),
               source.substr(start, current - start));
      return;
    }

    if (c == '\\') {
      const auto escapeStart = p_current;
      advance();
      if (isAtEnd()) {
        break;
      }
      if (isLineBreak(peek())) {
        lexerError(p_current, "Newline in string literal");
      }
      const char escaped = advance();
      if (!isValidEscape(escaped)) {
        lexerError(escapeStart, "Invalid escape in string literal");
      }
    } else if (isLineBreak(c)) {
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
    const auto escapeStart = p_current;
    advance();
    if (isAtEnd()) {
      lexerError(p_start, "Unterminated character literal");
    }
    const char escaped = advance();
    if (!isValidEscape(escaped)) {
      lexerError(escapeStart, "Invalid escape in character literal");
    }
  } else {
    advanceStringScalar();
  }

  if (peek() != '\'') {
    lexerError(p_start, "Character literal must contain exactly one character");
  }

  advance();
  addToken(TokenType::CharacterLiteral, span(),
           source.substr(start, current - start));
}
