// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <util/pstream.hpp>
#include <format>

namespace cloth {

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

/**
 * @brief Scans the source and writes a table of its tokens.
 */
void printLexer(Lexer &lexer, std::ostream &out) {
	lexer.scan();

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
	rows.reserve(lexer.get_tokens().size());
	for (const auto &token : lexer.get_tokens()) {
		const auto &row = rows.emplace_back(quoteLexeme(token.get_lexeme()), token.type_name(), token.get_span().to_string());
		lexemeWidth = std::max(lexemeWidth, row.lexeme.size());
		typeWidth = std::max(typeWidth, row.type.size());
		spanWidth = std::max(spanWidth, row.span.size());
	}

	const auto printRow = [&](std::string_view lexeme, std::string_view type, std::string_view spanText) {
		out << std::format("{:<{}}  {:<{}}  {}\n", lexeme, lexemeWidth, type, typeWidth, spanText);
	};
	printRow(lexemeHeader, typeHeader, spanHeader);
	printRow(std::string(lexemeWidth, '-'), std::string(typeWidth, '-'), std::string(spanWidth, '-'));
	for (const auto &row : rows) {
		printRow(row.lexeme, row.type, row.span);
	}

	out << "Count: " << lexer.get_tokens().size();
}

}
