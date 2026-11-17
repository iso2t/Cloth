// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <utility>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/struct_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <parser/type/type_parameters.hpp>
#include <parser/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

StructDeclaration StructParser::parse (const Token &keyword, std::optional<Token> visibility) {
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
			p_parser.expect(Operator::o_SEMICOLON);
			p_parser.skipNewLines();
			rejectFunction();
			if (!atMember() && !p_parser.startsTypeDeclaration()) {
				throw SyntaxErrors::expected(std::format("a '{}', '{}', or '{}' declaration after the fields", cloth::to_string(Keyword::k_MT),
				                                         cloth::to_string(Keyword::k_OPERATOR), cloth::to_string(Keyword::k_TYPEDEF)),
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

std::vector<StructFieldDeclaration> StructParser::parseFields () {
	return p_parser.p_delimitedParser.commaSeparated(
	        Operator::o_BRACE_CLOSE,
	        [this] () -> bool {
		        return p_parser.isSemiColon(p_parser.peek()) || atMember() || p_parser.startsTypeDeclaration() ||
		               p_parser.atKeyword(Keyword::k_FN, memberOffset());
	        },
	        [this] () -> StructFieldDeclaration {
		        rejectFunction();
		        std::optional<Token> opt_privateKeyword = p_parser.atKeyword(Keyword::k_PRIVATE) ? std::make_optional(p_parser.advance()) :
		                                                                                           std::nullopt;
		        TypeReference        tr_type            = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
		        const Token          t_name             = p_parser.expect(TokenType::Identifier, "a struct field name");
		        return { std::move(tr_type), t_name, std::move(opt_privateKeyword) };
	        });
}

std::vector<MethodDeclaration> StructParser::parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) {
	std::vector<MethodDeclaration> v_methods;
	while (!atEnd()) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		rejectFunction();
		if (p_parser.atKeyword(Keyword::k_MT)) {
			v_methods.push_back(p_parser.p_functionParser.parseMethod(std::nullopt, std::nullopt, std::nullopt, /*allowIntrinsic=*/false,
			                                                          /*allowTypeParameters=*/true));
		} else if (p_parser.atKeyword(Keyword::k_OPERATOR)) {
			v_methods.push_back(p_parser.p_functionParser.parseOperator());
		} else {
			throw SyntaxErrors::expected(std::format("a '{}' or '{}' declaration with inherited visibility", cloth::to_string(Keyword::k_MT),
			                                         cloth::to_string(Keyword::k_OPERATOR)),
			                             p_parser.peek());
		}
		const bool b_hasEquals = v_methods.back().body.get_equals() != nullptr;
		rejectFunction();
		const Token t_next = p_parser.peek();
		if (b_hasEquals && p_parser.isSemiColon(t_next)) {
			p_parser.advance();
		} else if (t_next.get_lexeme() != cloth::to_string(Operator::o_BRACE_CLOSE) && t_next.get_type() != TokenType::NewLine) {
			if (!b_hasEquals) {
				throw SyntaxErrors::expected(
				        std::format("a newline or '{}' after a brace-bodied method", cloth::to_string(Operator::o_BRACE_CLOSE)), t_next);
			}
			throw SyntaxErrors::newline(t_next);
		}
		p_parser.skipNewLines();
	}
	return v_methods;
}

void StructParser::rejectFunction () {
	const int i_offset = memberOffset();
	if (p_parser.atKeyword(Keyword::k_FN, i_offset)) {
		throw SyntaxErrors::functionInType(p_parser.peek(i_offset));
	}
}

int StructParser::memberOffset () {
	return p_parser.atKeyword(Keyword::k_PUBLIC) ? 1 : 0;
}

bool StructParser::atMember () {
	const int i_offset = memberOffset();
	return p_parser.atKeyword(Keyword::k_MT, i_offset) || p_parser.atKeyword(Keyword::k_OPERATOR, i_offset);
}

bool StructParser::atEnd () {
	return p_parser.peek().get_lexeme() == cloth::to_string(Operator::o_BRACE_CLOSE);
}

}
