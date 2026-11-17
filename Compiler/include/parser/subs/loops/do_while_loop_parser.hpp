// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <parser/stmt/do_while_statement.hpp>
#include <parser/subs/loops/loop_parser.hpp>

namespace cloth {

class DoWhileLoopParser : public LoopParser {
    public:
	DoWhileLoopParser (Parser &parser, DelimitedParser &delimited) : LoopParser(parser, delimited), p_parser(parser) {
	}

	DoWhileStatement parseDoWhile ();

    private:
	Parser &p_parser;
};

}
