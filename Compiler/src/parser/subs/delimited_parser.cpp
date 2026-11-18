// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <token/token.hpp>

namespace cloth {

void DelimitedParser::expect (const Operator op) const {
	p_parser.expect(op);
}

bool DelimitedParser::at (const Operator op) const {
	return p_parser.peek().get_lexeme() == to_string(op);
}

void DelimitedParser::skipNewLines () const {
	p_parser.skipNewLines();
}

}
