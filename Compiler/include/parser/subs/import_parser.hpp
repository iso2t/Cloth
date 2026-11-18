// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/decl/import_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class ImportParser {
    public:
	explicit ImportParser (Parser &parser) : p_parser(parser) {
	}

	ImportDeclaration parse () const;

    private:
	Parser &p_parser;

	ImportSelection::Names names () const;
	Token                  identifierOnLine (const Token &keyword) const;
	bool                   onLine (const Token &token) const;
	bool                   at (Operator op) const;
};

}
