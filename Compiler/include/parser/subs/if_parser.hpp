// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/decl/declarations.hpp>
#include <ast/expr/expressions.hpp>

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
