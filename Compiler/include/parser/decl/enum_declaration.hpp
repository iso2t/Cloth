// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <parser/expr/expression.hpp>
#include <parser/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct EnumFieldDeclaration {
	Token                name;
	TypeReference        type;
	std::optional<Token> visibility;
	ExpressionPtr        defaultValue;
};

struct EnumCaseDeclaration {
	Token                      name;
	std::vector<ExpressionPtr> values;
};

struct EnumDeclaration : TypeDeclaration {
	Token                             keyword;
	std::vector<EnumFieldDeclaration> fields;
	std::vector<EnumCaseDeclaration>  cases;
	std::vector<MethodDeclaration>    operators;

	EnumDeclaration (const Token &keyword, const Token &name, std::optional<Token> visibility, std::vector<EnumFieldDeclaration> fields,
	                 std::vector<EnumCaseDeclaration> cases, std::vector<MethodDeclaration> operators, TokenSpan span,
	                 std::vector<TypeDeclarationPtr> nestedTypes = {}) :
	        TypeDeclaration(name, std::move(visibility), span, std::move(nestedTypes)),
	        keyword(keyword),
	        fields(std::move(fields)),
	        cases(std::move(cases)),
	        operators(std::move(operators)) {
	}
};

}
