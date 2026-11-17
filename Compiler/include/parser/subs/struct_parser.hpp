// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class StructParser {
    public:
	explicit StructParser (Parser &parser) : p_parser(parser) {
	}

	StructDeclaration parse (const Token &keyword, std::optional<Token> visibility);

    private:
	Parser &p_parser;

	std::vector<StructFieldDeclaration> parseFields ();
	std::vector<MethodDeclaration>      parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes);
	void                                rejectFunction ();
	int                                 memberOffset ();
	bool                                atMember ();
	bool                                atEnd ();
	bool                                startsTypeDeclaration ();
};

}
