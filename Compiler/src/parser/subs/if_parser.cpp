// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/parser.hpp>
#include <parser/stmt/statement.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/if_parser.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

IfExpression IfParser::parseIf (bool allowWhileTrailer) {
	const Token                              t_keyword = p_parser.advance();
	std::vector<ConditionalBranch>           v_branches;
	std::optional<std::vector<StatementPtr>> opt_otherwise;
	std::optional<TokenSpan>                 opt_otherwiseSpan;
	while (true) {
		ExpressionPtr ep_condition = p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr {
			p_parser.skipNewLines();
			ExpressionPtr ep_expression = p_parser.p_expressionParser.parseExpression();
			p_parser.skipNewLines();
			return ep_expression;
		});
		p_parser.skipNewLines();
		BlockBody       bb_branch = parseBranchBody(allowWhileTrailer);
		const TokenSpan ts_branch = TokenSpan::span_through(ep_condition->get_span(), bb_branch.span);
		v_branches.push_back({ std::move(ep_condition), std::move(bb_branch.statements), ts_branch });

		int i_elseOffset = 0;
		while (p_parser.peek(i_elseOffset).get_type() == TokenType::NewLine) {
			++i_elseOffset;
		}
		if (!p_parser.atKeyword(Keyword::k_ELSE, i_elseOffset)) {
			break;
		}
		for (int i_skipped = 0; i_skipped <= i_elseOffset; ++i_skipped) {
			p_parser.advance();
		}
		p_parser.skipNewLines();
		if (!p_parser.atKeyword(Keyword::k_IF)) {
			BlockBody bb_otherwise = parseBranchBody(allowWhileTrailer);
			opt_otherwise          = std::move(bb_otherwise.statements);
			opt_otherwiseSpan      = bb_otherwise.span;
			break;
		}
		p_parser.advance();
		p_parser.skipNewLines();
	}

	const TokenSpan ts_span = TokenSpan::span_through(t_keyword.get_span(), p_parser.previousStatementEnd());
	return { t_keyword, std::move(v_branches), std::move(opt_otherwise), ts_span, opt_otherwiseSpan };
}

BlockBody IfParser::parseBranchBody (bool allowWhileTrailer) {
	const Token        t_start  = p_parser.peek();
	const std::string &s_lexeme = t_start.get_lexeme();
	if (s_lexeme == cloth::to_string(Operator::o_BRACE_OPEN)) {
		return p_parser.parseBlock();
	}
	if (t_start.get_type() == TokenType::EndOfFile || p_parser.atKeyword(Keyword::k_ELSE) || s_lexeme == cloth::to_string(Operator::o_BRACE_CLOSE) ||
	    s_lexeme == cloth::to_string(Operator::o_SEMICOLON)) {
		throw cde::SyntaxErrors::expected(std::format("a conditional body ('{}' or a statement)", cloth::to_string(Operator::o_BRACE_OPEN)), t_start);
	}
	return p_parser.parseUnbracedBody(allowWhileTrailer);
}

}
