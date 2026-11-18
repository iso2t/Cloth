// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include <ast/stmt/statement.hpp>

namespace cloth {

class Parser;

class BindingsParser {
    public:
	explicit BindingsParser (Parser &parser) : p_parser(parser) {
	}

	std::vector<StatementPtr> parseBindings (bool allowWhileTrailer = false) const;

    private:
	Parser &p_parser;
};

}
