// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/stmt/while_statement.hpp>
#include <parser/subs/loops/loop_parser.hpp>

namespace cloth {

class WhileLoopParser : public LoopParser {
    public:
	using LoopParser::LoopParser;

	WhileStatement parseWhile (bool allowWhileTrailer = false) const;
};

}
