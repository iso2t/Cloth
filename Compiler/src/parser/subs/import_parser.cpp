// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/source/Visibility.rst

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ast/decl/import_declaration.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/import_parser.hpp>
#include <token/token.hpp>

namespace cloth {

ImportDeclaration ImportParser::parse () const {
	const Token        t_keyword = p_parser.advance();
	std::vector        v_components{ pathIdentifier(t_keyword) };
	std::vector<Token> v_dots;
	while (at(ODot) && canContinueAfter(t_keyword)) {
		v_dots.push_back(p_parser.advance());
		v_components.push_back(pathIdentifier(t_keyword));
	}
	if (!canContinueAfter(t_keyword)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' on the same line as the import path", toString(OColon)), p_parser.peek());
	}
	const Token t_colon = p_parser.expect(OColon);
	p_parser.skipNewLines();
	ImportSelection is_selection = at(OMul) ? ImportSelection{ ImportSelection::Wildcard{ p_parser.advance() } } : ImportSelection{ names() };

	std::optional<Token> opt_terminator;
	if (p_parser.explicitSemicolons()) {
		opt_terminator = p_parser.requireSemicolon("import");
	} else if (canContinueAfter(p_parser.previous())) {
		const Token next = p_parser.peek();
		if (Parser::isSemiColon(next)) {
			opt_terminator = p_parser.advance();
		} else if (next.getType() != TokenType::NewLine && next.getType() != TokenType::EndOfFile) {
			throw cde::SyntaxErrors::newline(next);
		}
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
		                                          std::format("an imported declaration name or '{}' as the entire selection", toString(OMul)) :
		                                          std::format("an imported declaration name after '{}'", toString(OComma));
		const Token       t_name        = p_parser.expect(TokenType::Identifier, s_description);
		if (const auto [fst, snd] = m_seen.try_emplace(t_name.getNameKey(), t_name); !snd) {
			throw cde::ImportErrors::duplicateSelection(fst->second, t_name);
		}
		v_names.push_back(t_name);
		if (!canContinueAfter(t_name) || !at(OComma)) {
			break;
		}
		v_commas.push_back(p_parser.advance());
		p_parser.skipNewLines();
	}
	return { .names = std::move(v_names), .commas = std::move(v_commas) };
}

Token ImportParser::pathIdentifier (const Token &keyword) const {
	if (!canContinueAfter(keyword)) {
		throw cde::SyntaxErrors::expected(std::format("an import path identifier on the same line as '{}'", keyword.getLexeme()), p_parser.peek());
	}
	return p_parser.expect(TokenType::Identifier, "an import path identifier");
}

bool ImportParser::canContinueAfter (const Token &token) const {
	// Block comments can span lines without emitting newline tokens.
	return p_parser.explicitSemicolons() || p_parser.peek().getSpan().startLine == token.getSpan().endLine;
}

bool ImportParser::at (const Operator op) const {
	const Token t_next = p_parser.peek();
	return t_next.getType() == TokenType::Operator && t_next.getLexeme() == toString(op);
}

}
