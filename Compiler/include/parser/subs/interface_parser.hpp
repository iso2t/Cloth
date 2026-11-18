// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class InterfaceParser {
    public:
	explicit InterfaceParser (Parser &parser) : p_parser(parser) {
	}

	InterfaceDeclaration parse (const Token &keyword, const Token &kind, std::optional<Token> visibility) const;

    private:
	Parser &p_parser;

	std::vector<InterfaceMethodDeclaration> parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) const;
};

}
