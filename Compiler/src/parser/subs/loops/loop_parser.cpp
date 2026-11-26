// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst

#include <format>
#include <string>
#include <utility>

#include <ast/decl/declarations.hpp>
#include <ast/stmt/loop_control.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/loops/loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

LoopBody LoopParser::parseLoopBody (const Token &keyword, bool allowWhileTrailer) const {
	p_parser.skipNewLines();
	const Token        t_bodyStart = p_parser.peek();
	const std::string &s_lexeme    = t_bodyStart.getLexeme();
	if (s_lexeme == toString(OBraceOpen)) {
		BlockBody bb_block = p_parser.parseBlock();
		return { .statements = std::move(bb_block.statements), .braced = true, .span = bb_block.span };
	}
	if (p_parser.explicitSemicolons() && p_parser.isSemiColon(t_bodyStart)) {
		throw cde::SyntaxException{ cde::DiagnosticCode::ExpectedToken, "Expected a loop body", t_bodyStart.getSpan(),
			                    std::format("Write '{}{}' for an empty loop body.", toString(OBraceOpen), toString(OBraceClose)) };
	}
	if (t_bodyStart.getType() == TokenType::EndOfFile || s_lexeme == toString(OBraceClose) || s_lexeme == toString(OSemicolon)) {
		throw cde::SyntaxErrors::expected(std::format("a '{}' loop body ('{}' or a statement)", keyword.getLexeme(), toString(OBraceOpen)),
		                                  t_bodyStart);
	}

	BlockBody bb_body = p_parser.parseUnbracedBody(allowWhileTrailer);
	return { .statements = std::move(bb_body.statements), .braced = false, .span = bb_body.span };
}

}
