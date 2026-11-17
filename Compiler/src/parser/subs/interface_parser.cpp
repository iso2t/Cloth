// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/interface_declaration.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/interface_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <parser/type/type_parameters.hpp>
#include <parser/type/type_reference.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

InterfaceDeclaration InterfaceParser::parse (const Token &keyword, const Token &kind, std::optional<Token> visibility) {
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

std::vector<InterfaceMethodDeclaration> InterfaceParser::parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) {
	std::vector<InterfaceMethodDeclaration> v_methods;
	p_parser.skipNewLines();
	while (p_parser.peek().get_lexeme() != cloth::to_string(Operator::o_BRACE_CLOSE)) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		const int i_functionOffset = p_parser.atKeyword(Keyword::k_PUBLIC) ? 1 : 0;
		if (p_parser.atKeyword(Keyword::k_FN, i_functionOffset)) {
			throw cde::SyntaxErrors::functionInType(p_parser.peek(i_functionOffset));
		}
		if (!p_parser.atKeyword(Keyword::k_MT)) {
			throw cde::SyntaxErrors::expected(std::format("an interface '{}' declaration", cloth::to_string(Keyword::k_MT)), p_parser.peek());
		}
		v_methods.push_back(p_parser.p_functionParser.parseInterfaceMethod());
		const FunctionBody *fb_body = v_methods.back().executableBody();
		const bool          b_block = fb_body && std::holds_alternative<BlockBody>(fb_body->value);
		const Token         t_next  = p_parser.peek();
		if (!b_block && p_parser.isSemiColon(t_next)) {
			p_parser.advance();
		} else if (t_next.get_type() != TokenType::NewLine && t_next.get_lexeme() != cloth::to_string(Operator::o_BRACE_CLOSE)) {
			if (b_block) {
				throw cde::SyntaxErrors::expected(std::format("a newline or '{}' after a block default", cloth::to_string(Operator::o_BRACE_CLOSE)),
				                             t_next);
			}
			throw cde::SyntaxErrors::newline(t_next);
		}
		p_parser.skipNewLines();
	}
	return v_methods;
}

}
