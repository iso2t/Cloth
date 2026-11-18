// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class StructParser {
    public:
	explicit StructParser (Parser &parser) : p_parser(parser) {
	}

	StructDeclaration parse (const Token &keyword, std::optional<Token> visibility) const;

    private:
	Parser &p_parser;

	std::vector<StructFieldDeclaration> parseFields () const;
	std::vector<MethodDeclaration>      parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) const;
	void                                rejectFunction () const;
	int                                 memberOffset () const;
	bool                                atMember () const;
	bool                                atEnd () const;
};

}
