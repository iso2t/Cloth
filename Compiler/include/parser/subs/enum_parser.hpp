// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class EnumParser {
    public:
	explicit EnumParser (Parser &parser) : p_parser(parser) {
	}

	EnumDeclaration parse (const Token &keyword, std::optional<Token> visibility) const;

    private:
	Parser &p_parser;

	EnumFieldDeclaration           parseField () const;
	EnumCaseDeclaration            parseCase () const;
	std::vector<MethodDeclaration> parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) const;
	ExpressionPtr                  value () const;
	bool                           at (Operator op) const;
};

}
