// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <parser/stmt/for_statement.hpp>
#include <parser/subs/loops/loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

class ForLoopParser : public LoopParser {
    public:
	ForLoopParser (Parser &parser, DelimitedParser &delimited) : LoopParser(parser, delimited), p_parser(parser) {
	}

	ForStatement parseFor (bool allowWhileTrailer = false);

    private:
	Parser &p_parser;

	ForStepper parseForStepper (const Token &counter);
};

}
