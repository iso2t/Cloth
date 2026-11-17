// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <string>
#include <utility>

#include <parser/decl/declarations.hpp>
#include <parser/parser.hpp>
#include <parser/stmt/loop_control.hpp>
#include <parser/subs/loops/loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

LoopBody LoopParser::parseLoopBody (const Token &keyword, bool allowWhileTrailer) {
	p_parser.skipNewLines();
	const Token        t_bodyStart = p_parser.peek();
	const std::string &s_lexeme    = t_bodyStart.get_lexeme();
	if (s_lexeme == cloth::to_string(Operator::o_BRACE_OPEN)) {
		BlockBody bb_block = p_parser.parseBlock();
		return { std::move(bb_block.statements), true, bb_block.span };
	}
	if (t_bodyStart.get_type() == TokenType::EndOfFile || s_lexeme == cloth::to_string(Operator::o_BRACE_CLOSE) ||
	    s_lexeme == cloth::to_string(Operator::o_SEMICOLON)) {
		throw SyntaxErrors::expected(
		        std::format("a '{}' loop body ('{}' or a statement)", keyword.get_lexeme(), cloth::to_string(Operator::o_BRACE_OPEN)), t_bodyStart);
	}

	BlockBody bb_body = p_parser.parseUnbracedBody(allowWhileTrailer);
	return { std::move(bb_body.statements), false, bb_body.span };
}

}
