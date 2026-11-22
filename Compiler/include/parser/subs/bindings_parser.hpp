// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <ast/stmt/statement.hpp>
#include <ast/type/type_reference.hpp>

namespace cloth {

class Parser;

class BindingsParser {
    public:
	explicit BindingsParser (Parser &parser) : p_parser(parser) {
	}

	bool                         startsBinding () const;
	std::optional<TypeReference> parseBindingType () const;
	std::vector<StatementPtr>    parseBindings (bool allowWhileTrailer = false) const;

    private:
	Parser &p_parser;
};

}
