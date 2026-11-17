// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <parser/decl/declarations.hpp>
#include <parser/expr/expressions.hpp>

namespace cloth {

class Parser;

class IfParser {
    public:
	explicit IfParser (Parser &parser) : p_parser(parser) {
	}

	/** An else can follow newlines. A semicolon ends the chain. */
	IfExpression parseIf (bool allowWhileTrailer = false);

    private:
	Parser &p_parser;

	BlockBody parseBranchBody (bool allowWhileTrailer);
};

}
