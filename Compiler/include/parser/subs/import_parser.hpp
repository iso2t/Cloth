// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <parser/decl/import_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class ImportParser {
    public:
	explicit ImportParser (Parser &parser) : p_parser(parser) {
	}

	ImportDeclaration parse ();

    private:
	Parser &p_parser;

	ImportSelection::Names names ();
	Token                  identifierOnLine (const Token &keyword);
	bool                   onLine (const Token &token);
	bool                   at (Operator op);
};

}
