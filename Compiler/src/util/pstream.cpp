// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <lexer/lexer.hpp>
#include <token/token.hpp>
#include <util/pstream.hpp>

namespace cloth {

std::string quoteLexeme (const std::string_view lexeme) {
	std::string s_quoted(1, '"');
	s_quoted.reserve(lexeme.size() + 2);
	for (const char c_character : lexeme) {
		switch (c_character) {
		case '"':
		case '\\':
			s_quoted += '\\';
			s_quoted += c_character;
			break;
		case '\n':
			s_quoted += "\\n";
			break;
		case '\r':
			s_quoted += "\\r";
			break;
		case '\t':
			s_quoted += "\\t";
			break;
		default:
			const auto c_byte = static_cast<unsigned char>(c_character);
			if (c_byte < 0x20 || c_byte >= 0x7F) {
				s_quoted += std::format("\\x{:02X}", c_byte);
			} else {
				s_quoted += c_character;
			}
		}
	}
	s_quoted += '"';
	return s_quoted;
}

/**
 * @brief Scans the source and writes a table of its tokens.
 */
void printLexer (Lexer &lexer, std::ostream &out) {
	lexer.scan();
	const std::vector<Token> &tokens = lexer.getTokens();

	constexpr std::string_view s_lexemeHeader = "LEXEME";
	constexpr std::string_view s_typeHeader   = "TYPE";
	constexpr std::string_view s_spanHeader   = "SPAN";
	size_t                     st_lexemeWidth = s_lexemeHeader.size();
	size_t                     st_typeWidth   = s_typeHeader.size();
	size_t                     st_spanWidth   = s_spanHeader.size();

	struct Row {
		std::string      lexeme;
		std::string_view type;
		std::string      span;
	};

	std::vector<Row> v_rows;
	v_rows.reserve(tokens.size());
	for (const Token &t_token : tokens) {
		const Row &r_row = v_rows.emplace_back(quoteLexeme(t_token.getLexeme()), t_token.typeName(), t_token.getSpan().toString());
		st_lexemeWidth   = std::max(st_lexemeWidth, r_row.lexeme.size());
		st_typeWidth     = std::max(st_typeWidth, r_row.type.size());
		st_spanWidth     = std::max(st_spanWidth, r_row.span.size());
	}

	const auto printRow = [&] (std::string_view lexeme, std::string_view type, std::string_view spanText) {
		out << std::format("{:<{}}  {:<{}}  {}\n", lexeme, st_lexemeWidth, type, st_typeWidth, spanText);
	};
	printRow(s_lexemeHeader, s_typeHeader, s_spanHeader);
	printRow(std::string(st_lexemeWidth, '-'), std::string(st_typeWidth, '-'), std::string(st_spanWidth, '-'));
	for (const auto &[lexeme, type, span] : v_rows) {
		printRow(lexeme, type, span);
	}

	out << "Count: " << tokens.size();
}
}
