// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <token/token.hpp>

namespace cloth {

void DelimitedParser::expect (Operator op) {
	p_parser.expect(op);
}

bool DelimitedParser::at (Operator op) {
	return p_parser.peek().get_lexeme() == cloth::to_string(op);
}

void DelimitedParser::skipNewLines () {
	p_parser.skipNewLines();
}

}
