// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst

#include <format>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/loops/do_while_loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

DoWhileStatement DoWhileLoopParser::parseDoWhile () const {
	const Token t_keyword = p_parser.advance();
	LoopBody    lb_body   = parseLoopBody(t_keyword, /*allowWhileTrailer=*/true);

	if (!p_parser.atKeyword(KWhile)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' after a '{}' body", toString(KWhile), toString(KDo)), p_parser.peek());
	}

	const Token t_whileKeyword = p_parser.advance();
	const Token t_opening = p_parser.expect(OParenOpen);
	ExpressionPtr ep_condition = p_parser.p_expressionParser.parseExpression();
	const Token t_closing = p_parser.expect(OParenClose);
	p_parser.requireSemicolon("do-while condition");

	return { t_keyword, std::move(lb_body), t_whileKeyword, t_opening, std::move(ep_condition), t_closing };
}

}
