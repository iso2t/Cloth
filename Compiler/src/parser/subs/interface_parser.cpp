// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/types/Declarations.rst

#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <cde/syntax_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/interface_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {

InterfaceDeclaration InterfaceParser::parse (const Token &keyword, const Token &kind, std::optional<Token> visibility) const {
	const Token                      t_name         = p_parser.expect(TokenType::Identifier, "an interface name");
	std::optional<TypeParameterList> opt_parameters = p_parser.p_typeParser.parseTypeParameters();
	std::vector<TypeReference>       v_parents =
	        p_parser.p_typeParser.parseParents("an interface parent name", [] (TypeReference type, const Token &) -> TypeReference { return type; });
	std::vector<TypeDeclarationPtr> v_nestedTypes;
	p_parser.skipNewLines();
	std::vector<InterfaceMethodDeclaration> v_methods =
	        p_parser.p_delimitedParser.braces([this, &v_nestedTypes] () -> std::vector<InterfaceMethodDeclaration> { return parseMembers(v_nestedTypes); });
	const TokenSpan ts_span = TokenSpan::span_through((visibility ? *visibility : keyword).get_span(), p_parser.previous().get_span());
	return { keyword,
		 kind,
		 t_name,
		 std::move(visibility),
		 std::move(opt_parameters),
		 std::move(v_parents),
		 std::move(v_methods),
		 ts_span,
		 std::move(v_nestedTypes) };
}

std::vector<InterfaceMethodDeclaration> InterfaceParser::parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) const {
	std::vector<InterfaceMethodDeclaration> v_methods;
	p_parser.skipNewLines();
	while (p_parser.peek().get_lexeme() != to_string(oBraceClose)) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		std::optional<Token> opt_visibility = p_parser.parseVisibility();
		if (p_parser.atKeyword(kFn)) {
			throw cde::SyntaxErrors::functionInType(p_parser.peek());
		}
		if (!p_parser.atKeyword(kMt)) {
			throw cde::SyntaxErrors::expected(std::format("an interface '{}' declaration", to_string(kMt)), p_parser.peek());
		}
		v_methods.push_back(p_parser.p_functionParser.parseInterfaceMethod(std::move(opt_visibility)));
		const FunctionBody *fb_body = v_methods.back().executableBody();
		const bool          b_block = fb_body && std::holds_alternative<BlockBody>(fb_body->value);
		if (const Token t_next = p_parser.peek(); !b_block && p_parser.isSemiColon(t_next)) {
			p_parser.advance();
		} else if (t_next.get_type() != TokenType::NewLine && t_next.get_lexeme() != to_string(oBraceClose)) {
			if (b_block) {
				throw cde::SyntaxErrors::expected(std::format("a newline or '{}' after a block default", to_string(oBraceClose)), t_next);
			}
			throw cde::SyntaxErrors::newline(t_next);
		}
		p_parser.skipNewLines();
	}
	return v_methods;
}

}
