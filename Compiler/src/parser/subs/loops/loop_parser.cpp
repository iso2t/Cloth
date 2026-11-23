// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst

#include <format>
#include <string>
#include <utility>

#include <ast/decl/declarations.hpp>
#include <ast/stmt/loop_control.hpp>
#include <cde/syntax_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/loops/loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

LoopBody LoopParser::parseLoopBody (const Token &keyword, bool allowWhileTrailer) const {
	p_parser.skipNewLines();
	const Token        t_bodyStart = p_parser.peek();
	const std::string &s_lexeme    = t_bodyStart.get_lexeme();
	if (s_lexeme == to_string(oBraceOpen)) {
		BlockBody bb_block = p_parser.parseBlock();
		return { std::move(bb_block.statements), true, bb_block.span };
	}
	if (t_bodyStart.get_type() == TokenType::EndOfFile || s_lexeme == to_string(oBraceClose) || s_lexeme == to_string(oSemicolon)) {
		throw cde::SyntaxErrors::expected(std::format("a '{}' loop body ('{}' or a statement)", keyword.get_lexeme(), to_string(oBraceOpen)),
		                                  t_bodyStart);
	}

	BlockBody bb_body = p_parser.parseUnbracedBody(allowWhileTrailer);
	return { std::move(bb_body.statements), false, bb_body.span };
}

}
