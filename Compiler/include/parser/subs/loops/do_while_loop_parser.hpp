// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/stmt/do_while_statement.hpp>
#include <parser/subs/loops/loop_parser.hpp>

namespace cloth {

class DoWhileLoopParser : public LoopParser {
    public:
	using LoopParser::LoopParser;

	DoWhileStatement parseDoWhile () const;
};

}
