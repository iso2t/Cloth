// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/stmt/for_statement.hpp>
#include <parser/subs/loops/loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

class ForLoopParser : public LoopParser {
    public:
	using LoopParser::LoopParser;

	ForStatement parseFor (bool allowWhileTrailer = false) const;

    private:
	ForStepper parseForStepper (const Token &counter) const;
};

}
