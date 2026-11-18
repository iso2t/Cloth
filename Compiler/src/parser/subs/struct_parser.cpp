// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/struct_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

StructDeclaration StructParser::parse (const Token &keyword, std::optional<Token> visibility) const {
	const Token                         t_name             = p_parser.expect(TokenType::Identifier, "a struct name");
	std::optional<TypeParameterList>    opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
	std::vector<TypeDeclarationPtr>     v_nestedTypes;
	std::vector<StructFieldDeclaration> v_fields;
	std::vector<MethodDeclaration>      v_methods;
	p_parser.skipNewLines();
	p_parser.p_delimitedParser.braces([this, &v_nestedTypes, &v_fields, &v_methods] () -> void {
		p_parser.skipNewLines();
		while (p_parser.startsTypeDeclaration()) {
			v_nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
		}
		if (!atMember()) {
			v_fields = parseFields();
		}
		if (!v_fields.empty() && !atEnd()) {
			rejectFunction();
			p_parser.expect(oSemicolon);
			p_parser.skipNewLines();
			rejectFunction();
			if (!atMember() && !p_parser.startsTypeDeclaration()) {
				throw cde::SyntaxErrors::expected(std::format("a '{}', '{}', or '{}' declaration after the fields", to_string(kMt),
				                                              to_string(kOperator), to_string(kTypeDef)),
				                                  p_parser.peek());
			}
		}
		v_methods = parseMembers(v_nestedTypes);
	});
	const TokenSpan ts_span = TokenSpan::span_through((visibility ? *visibility : keyword).get_span(), p_parser.previous().get_span());
	return { keyword,
		 t_name,
		 std::move(v_fields),
		 std::move(visibility),
		 ts_span,
		 std::move(v_methods),
		 std::move(opt_typeParameters),
		 std::move(v_nestedTypes) };
}

std::vector<StructFieldDeclaration> StructParser::parseFields () const {
	return p_parser.p_delimitedParser.commaSeparated(
	        oBraceClose,
	        [this] () -> bool {
		        return p_parser.isSemiColon(p_parser.peek()) || atMember() || p_parser.startsTypeDeclaration() ||
		               p_parser.atKeyword(kFn, memberOffset());
	        },
	        [this] () -> StructFieldDeclaration {
		        rejectFunction();
		        std::optional<Token> opt_privateKeyword = p_parser.atKeyword(kPrivate) ? std::make_optional(p_parser.advance()) : std::nullopt;
		        TypeReference        tr_type            = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
		        const Token          t_name             = p_parser.expect(TokenType::Identifier, "a struct field name");
		        return { .type = std::move(tr_type), .name = t_name, .privateKeyword = std::move(opt_privateKeyword) };
	        });
}

std::vector<MethodDeclaration> StructParser::parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) const {
	std::vector<MethodDeclaration> v_methods;
	while (!atEnd()) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		rejectFunction();
		if (p_parser.atKeyword(kMt)) {
			v_methods.push_back(p_parser.p_functionParser.parseMethod(std::nullopt, std::nullopt, std::nullopt, /*allowIntrinsic=*/false,
			                                                          /*allowTypeParameters=*/true));
		} else if (p_parser.atKeyword(kOperator)) {
			v_methods.push_back(p_parser.p_functionParser.parseOperator());
		} else {
			throw cde::SyntaxErrors::expected(
			        std::format("a '{}' or '{}' declaration with inherited visibility", to_string(kMt), to_string(kOperator)), p_parser.peek());
		}
		const bool b_hasEquals = v_methods.back().body.get_equals() != nullptr;
		rejectFunction();
		if (const Token t_next = p_parser.peek(); b_hasEquals && p_parser.isSemiColon(t_next)) {
			p_parser.advance();
		} else if (t_next.get_lexeme() != to_string(oBraceClose) && t_next.get_type() != TokenType::NewLine) {
			if (!b_hasEquals) {
				throw cde::SyntaxErrors::expected(std::format("a newline or '{}' after a brace-bodied method", to_string(oBraceClose)), t_next);
			}
			throw cde::SyntaxErrors::newline(t_next);
		}
		p_parser.skipNewLines();
	}
	return v_methods;
}

void StructParser::rejectFunction () const {
	if (const int i_offset = memberOffset(); p_parser.atKeyword(kFn, i_offset)) {
		throw cde::SyntaxErrors::functionInType(p_parser.peek(i_offset));
	}
}

int StructParser::memberOffset () const {
	return p_parser.atKeyword(kPublic) ? 1 : 0;
}

bool StructParser::atMember () const {
	const int i_offset = memberOffset();
	return p_parser.atKeyword(kMt, i_offset) || p_parser.atKeyword(kOperator, i_offset);
}

bool StructParser::atEnd () const {
	return p_parser.peek().get_lexeme() == to_string(oBraceClose);
}

}
