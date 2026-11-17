// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <parser/decl/import_declaration.hpp>
#include <parser/parser.hpp>
#include <parser/subs/import_parser.hpp>
#include <token/token.hpp>

namespace cloth {

ImportDeclaration ImportParser::parse () {
	const Token        t_keyword = p_parser.advance();
	std::vector<Token> v_components{ identifierOnLine(t_keyword) };
	std::vector<Token> v_dots;
	while (at(Operator::o_DOT) && onLine(t_keyword)) {
		v_dots.push_back(p_parser.advance());
		v_components.push_back(identifierOnLine(t_keyword));
	}
	if (!onLine(t_keyword)) {
		throw SyntaxErrors::expected(std::format("'{}' on the same line as the import path", cloth::to_string(Operator::o_COLON)), p_parser.peek());
	}
	if (!at(Operator::o_COLON)) {
		throw SyntaxErrors::expected(std::format("'{}'", cloth::to_string(Operator::o_COLON)), p_parser.peek());
	}
	const Token t_colon = p_parser.advance();
	p_parser.skipNewLines();
	ImportSelection is_selection = at(Operator::o_MULTIPLY) ? ImportSelection{ ImportSelection::Wildcard{ p_parser.advance() } } :
	                                                          ImportSelection{ names() };

	const Token          t_last         = p_parser.previous();
	const bool           b_sameLine     = onLine(t_last);
	const Token          t_next         = p_parser.peek();
	std::optional<Token> opt_terminator = b_sameLine && p_parser.isSemiColon(t_next) ? std::make_optional(p_parser.advance()) : std::nullopt;
	if (!opt_terminator && b_sameLine && t_next.get_type() != TokenType::NewLine && t_next.get_type() != TokenType::EndOfFile) {
		throw SyntaxErrors::newline(t_next);
	}
	return { t_keyword, { std::move(v_components), std::move(v_dots) }, t_colon, std::move(is_selection), std::move(opt_terminator) };
}

ImportSelection::Names ImportParser::names () {
	using SeenNames = std::unordered_map<std::string, Token>;
	std::vector<Token> v_names;
	std::vector<Token> v_commas;
	SeenNames          m_seen;
	while (true) {
		const std::string s_description =
		        v_names.empty() ? std::format("an imported declaration name or '{}' as the entire selection", cloth::to_string(Operator::o_MULTIPLY)) :
		                          std::format("an imported declaration name after '{}'", cloth::to_string(Operator::o_COMMA));
		const Token                                t_name      = p_parser.expect(TokenType::Identifier, s_description);
		const std::pair<SeenNames::iterator, bool> p_insertion = m_seen.try_emplace(t_name.get_lexeme(), t_name);
		if (!p_insertion.second) {
			throw ImportErrors::duplicateSelection(p_insertion.first->second, t_name);
		}
		v_names.push_back(t_name);
		if (!onLine(t_name) || !at(Operator::o_COMMA)) {
			break;
		}
		v_commas.push_back(p_parser.advance());
		p_parser.skipNewLines();
	}
	return { std::move(v_names), std::move(v_commas) };
}

Token ImportParser::identifierOnLine (const Token &keyword) {
	if (!onLine(keyword)) {
		throw SyntaxErrors::expected(std::format("an import path identifier on the same line as '{}'", keyword.get_lexeme()), p_parser.peek());
	}
	return p_parser.expect(TokenType::Identifier, "an import path identifier");
}

bool ImportParser::onLine (const Token &token) {
	// Block comments can span lines without emitting newline tokens.
	return p_parser.peek().get_span().startLine == token.get_span().endLine;
}

bool ImportParser::at (Operator op) {
	const Token t_next = p_parser.peek();
	return t_next.get_type() == TokenType::Operator && t_next.get_lexeme() == cloth::to_string(op);
}

}
