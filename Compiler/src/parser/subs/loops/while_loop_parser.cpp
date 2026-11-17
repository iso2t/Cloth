// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <utility>

#include <parser/expr/expression.hpp>
#include <parser/parser.hpp>
#include <parser/stmt/while_statement.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/loops/while_loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

WhileStatement WhileLoopParser::parseWhile (bool allowWhileTrailer) {
	const Token t_keyword = p_parser.advance();
	p_parser.skipNewLines();
	const Token t_opening = p_parser.expect(Operator::o_PAREN_OPEN);
	p_parser.skipNewLines();
	ExpressionPtr ep_condition = p_parser.p_expressionParser.parseExpression();
	p_parser.skipNewLines();
	const Token t_closing = p_parser.expect(Operator::o_PAREN_CLOSE);
	return { t_keyword, t_opening, std::move(ep_condition), t_closing, parseLoopBody(t_keyword, allowWhileTrailer) };
}

}
