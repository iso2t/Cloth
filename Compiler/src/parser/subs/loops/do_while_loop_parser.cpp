// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <parser/parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/loops/do_while_loop_parser.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

DoWhileStatement DoWhileLoopParser::parseDoWhile () {
	const Token t_keyword = p_parser.advance();
	LoopBody    lb_body   = parseLoopBody(t_keyword, /*allowWhileTrailer=*/true);

	p_parser.skipNewLines();
	if (!p_parser.atKeyword(Keyword::k_WHILE)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' after a '{}' body", cloth::to_string(Keyword::k_WHILE), cloth::to_string(Keyword::k_DO)),
		                                  p_parser.peek());
	}

	const Token t_whileKeyword = p_parser.advance();
	p_parser.skipNewLines();
	const Token t_opening = p_parser.expect(Operator::o_PAREN_OPEN);
	p_parser.skipNewLines();
	ExpressionPtr ep_condition = p_parser.p_expressionParser.parseExpression();
	p_parser.skipNewLines();
	const Token t_closing = p_parser.expect(Operator::o_PAREN_CLOSE);

	return { t_keyword, std::move(lb_body), t_whileKeyword, t_opening, std::move(ep_condition), t_closing };
}

}
