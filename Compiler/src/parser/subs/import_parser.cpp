// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ast/decl/import_declaration.hpp>
#include <import_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/import_parser.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

ImportDeclaration ImportParser::parse () const {
	const Token        t_keyword = p_parser.advance();
	std::vector        v_components{ identifierOnLine(t_keyword) };
	std::vector<Token> v_dots;
	while (at(oDot) && onLine(t_keyword)) {
		v_dots.push_back(p_parser.advance());
		v_components.push_back(identifierOnLine(t_keyword));
	}
	if (!onLine(t_keyword)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' on the same line as the import path", to_string(oColon)), p_parser.peek());
	}
	const Token t_colon = p_parser.expect(oColon);
	p_parser.skipNewLines();
	ImportSelection is_selection = at(oMul) ? ImportSelection{ ImportSelection::Wildcard{ p_parser.advance() } } : ImportSelection{ names() };

	const Token          t_last         = p_parser.previous();
	const bool           b_sameLine     = onLine(t_last);
	const Token          t_next         = p_parser.peek();
	std::optional<Token> opt_terminator = b_sameLine && p_parser.isSemiColon(t_next) ? std::make_optional(p_parser.advance()) : std::nullopt;
	if (!opt_terminator && b_sameLine && t_next.get_type() != TokenType::NewLine && t_next.get_type() != TokenType::EndOfFile) {
		throw cde::SyntaxErrors::newline(t_next);
	}
	return { .keyword    = t_keyword,
		 .path       = { .components = std::move(v_components), .dots = std::move(v_dots) },
		 .colon      = t_colon,
		 .selection  = std::move(is_selection),
		 .terminator = std::move(opt_terminator) };
}

ImportSelection::Names ImportParser::names () const {
	using SeenNames = std::unordered_map<std::string, Token>;
	std::vector<Token> v_names;
	std::vector<Token> v_commas;
	SeenNames          m_seen;
	while (true) {
		const std::string s_description = v_names.empty() ?
		                                          std::format("an imported declaration name or '{}' as the entire selection", to_string(oMul)) :
		                                          std::format("an imported declaration name after '{}'", to_string(oComma));
		const Token       t_name        = p_parser.expect(TokenType::Identifier, s_description);
		if (const auto [fst, snd] = m_seen.try_emplace(t_name.get_lexeme(), t_name); !snd) {
			throw cde::ImportErrors::duplicateSelection(fst->second, t_name);
		}
		v_names.push_back(t_name);
		if (!onLine(t_name) || !at(oComma)) {
			break;
		}
		v_commas.push_back(p_parser.advance());
		p_parser.skipNewLines();
	}
	return { .names = std::move(v_names), .commas = std::move(v_commas) };
}

Token ImportParser::identifierOnLine (const Token &keyword) const {
	if (!onLine(keyword)) {
		throw cde::SyntaxErrors::expected(std::format("an import path identifier on the same line as '{}'", keyword.get_lexeme()), p_parser.peek());
	}
	return p_parser.expect(TokenType::Identifier, "an import path identifier");
}

bool ImportParser::onLine (const Token &token) const {
	// Block comments can span lines without emitting newline tokens.
	return p_parser.peek().get_span().startLine == token.get_span().endLine;
}

bool ImportParser::at (const Operator op) const {
	const Token t_next = p_parser.peek();
	return t_next.get_type() == TokenType::Operator && t_next.get_lexeme() == to_string(op);
}

}
