// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/enum_declaration.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <parser/expr/expression.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class EnumParser {
    public:
	explicit EnumParser (Parser &parser) : p_parser(parser) {
	}

	EnumDeclaration parse (const Token &keyword, std::optional<Token> visibility);

    private:
	Parser &p_parser;

	EnumFieldDeclaration           parseField ();
	EnumCaseDeclaration            parseCase ();
	std::vector<MethodDeclaration> parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes);
	ExpressionPtr                  value ();
	bool                           at (Operator op);
};

}
