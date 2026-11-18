// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/type/type_reference.hpp>
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
	std::optional<Token>       visibility = std::nullopt;
};

struct EnumDeclaration : TypeDeclaration {
	Token                             keyword;
	std::vector<EnumFieldDeclaration> fields;
	std::vector<EnumCaseDeclaration>  cases;
	std::vector<MethodDeclaration>    operators;

	EnumDeclaration (const Token &keyword, const Token &name, std::optional<Token> visibility, std::vector<EnumFieldDeclaration> fields,
	                 std::vector<EnumCaseDeclaration> cases, std::vector<MethodDeclaration> operators, const TokenSpan &span,
	                 std::vector<TypeDeclarationPtr> nestedTypes = {}) :
	        TypeDeclaration(name, std::move(visibility), span, std::move(nestedTypes)),
	        keyword(keyword),
	        fields(std::move(fields)),
	        cases(std::move(cases)),
	        operators(std::move(operators)) {
	}
};

}
