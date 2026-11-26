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
#include <diagnostics.hpp>
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
	std::vector<InterfaceMethodDeclaration> v_methods;
	if (!p_parser.consumeEmptyTypeHeader("interface header")) {
		v_methods = p_parser.p_delimitedParser.braces([this, &v_nestedTypes] () { return parseMembers(v_nestedTypes); });
	}
	const TokenSpan ts_span = TokenSpan::spanThrough((visibility ? *visibility : keyword).getSpan(), p_parser.previous().getSpan());
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
	p_parser.parseDeclarationItems(/*braced=*/true, [&] (const PublicBlockOrigin &publicBlock) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration(publicBlock));
			return;
		}
		std::optional<Token> opt_visibility = p_parser.parseVisibility();
		if (p_parser.atKeyword(KFn)) {
			throw cde::SyntaxErrors::functionInType(p_parser.peek());
		}
		if (!p_parser.atKeyword(KMt)) {
			throw cde::SyntaxErrors::expected(std::format("an interface '{}' declaration", toString(KMt)), p_parser.peek());
		}
		InterfaceMethodDeclaration &method = v_methods.emplace_back(p_parser.p_functionParser.parseInterfaceMethod(std::move(opt_visibility)));
		method.publicBlock                 = publicBlock;
		const FunctionBody *fb_body        = method.executableBody();
		const bool          b_block = fb_body && std::holds_alternative<BlockBody>(fb_body->value);
		if (!b_block) {
			p_parser.requireSemicolon("interface method");
		}
	});
	return v_methods;
}

}
