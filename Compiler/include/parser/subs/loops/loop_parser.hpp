// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/stmt/loop_control.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class LoopParser {
    public:
	explicit LoopParser (Parser &parser) : p_parser(parser) {
	}

	LoopBody parseLoopBody (const Token &keyword, bool allowWhileTrailer = false) const;

    protected:
	Parser &p_parser;
};

}
