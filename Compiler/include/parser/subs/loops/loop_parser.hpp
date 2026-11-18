// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/stmt/loop_control.hpp>
#include <token/token.hpp>

namespace cloth {

class DelimitedParser;
class Parser;

class LoopParser {
    public:
	LoopParser (Parser &parser, DelimitedParser &delimited) : p_parser(parser), dp_delimited(delimited) {
	}

	DelimitedParser &get_delimited () const {
		return dp_delimited;
	}

	LoopBody parseLoopBody (const Token &keyword, bool allowWhileTrailer = false);

    private:
	Parser          &p_parser;
	DelimitedParser &dp_delimited;
};

}
