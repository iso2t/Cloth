// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/bindings_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/loops/for_loop_parser.hpp>
#include <token/token.hpp>

namespace cloth {

ForStatement ForLoopParser::parseFor (bool allowWhileTrailer) const {
	const Token t_keyword = p_parser.advance();
	const Token t_opening = p_parser.expect(OParenOpen);
	const Token                  t_start  = p_parser.peek();
	std::optional<TypeReference> opt_type = p_parser.p_bindingParser.parseBindingType();
	const Token t_name = p_parser.expect(TokenType::Identifier, "a loop binding name");
	ForBinding  fb_binding{ .start = t_start, .type = std::move(opt_type), .name = t_name };
	const Token t_arrow = p_parser.expect(OPointerLeft);

	const TokenSpan ts_sourceStart = p_parser.peek().getSpan();
	ExpressionPtr   ep_first       = p_parser.p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/true);
	const TokenSpan ts_firstSpan   = TokenSpan::spanThrough(ts_sourceStart, p_parser.previous().getSpan());

	const Token        t_afterFirst = p_parser.peek();
	const std::string &s_afterFirst = t_afterFirst.getLexeme();
	const bool         b_isRange    = s_afterFirst == toString(ORange) || s_afterFirst == toString(ORangeInclusive);
	ForSource          fs_source{ ForSource::Elements{ .expression = ep_first, .span = ts_firstSpan } };
	if (b_isRange) {
		const Token t_delimiter = p_parser.advance();
		ExpressionPtr ep_end = p_parser.p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/true);
		fs_source.value      = ForSource::Range{ .expression = std::make_shared<BinaryExpression>(std::move(ep_first), t_delimiter, std::move(ep_end)),
			                                 .span       = TokenSpan::spanThrough(ts_sourceStart, p_parser.previous().getSpan()) };
	}

	std::optional<Token> opt_separator = p_parser.isSemiColon(p_parser.peek()) ? std::make_optional(p_parser.advance()) : std::nullopt;
	if (opt_separator && !b_isRange) {
		throw cde::SyntaxErrors::expected(
		        std::format("'{}' after a foreach source; steppers are only allowed on numeric ranges", toString(OParenClose)), *opt_separator);
	}
	std::optional<ForStepper> opt_stepper = opt_separator ? std::make_optional(parseForStepper(t_name)) : std::nullopt;
	if (b_isRange && !opt_separator && p_parser.peek().getLexeme() != toString(OParenClose)) {
		throw cde::SyntaxErrors::expected(
		        std::format("'{}' before a range stepper or '{}' after the range", toString(OSemicolon), toString(OParenClose)), p_parser.peek());
	}
	const Token t_closing = p_parser.expect(OParenClose);
	LoopBody    lb_body   = parseLoopBody(t_keyword, allowWhileTrailer);
	return { t_keyword, t_opening,         std::move(fb_binding), t_arrow, std::move(fs_source), std::move(opt_separator), std::move(opt_stepper),
		 t_closing, std::move(lb_body) };
}

ForStepper ForLoopParser::parseForStepper (const Token &counter) const {
	const std::string s_description = std::format("the loop counter '{}' as the stepper target", counter.getLexeme());
	const Token       t_target      = p_parser.expect(TokenType::Identifier, s_description);
	if (t_target.getNameKey() != counter.getNameKey()) {
		throw cde::SyntaxErrors::expected(s_description, t_target);
	}

	const Token        t_operator = p_parser.peek();
	const std::string &s_operator = t_operator.getLexeme();
	if (t_operator.getType() != TokenType::Operator || !Parser::ASSIGNMENT_OPERATORS.contains(s_operator) || s_operator == toString(OFinalAssign)) {
		throw cde::SyntaxErrors::expected(std::format("a counter assignment/update operator ({}, {}, {}, {}, {}, {}, {} or {})", toString(OAssign),
		                                              toString(OPlusEqual), toString(OMinusEqual), toString(OMulEqual), toString(ODivEqual),
		                                              toString(OModEqual), toString(OPlusPlus), toString(OMinusMinus)),
		                                  t_operator);
	}
	p_parser.advance();
	const bool    b_postfix = s_operator == toString(OPlusPlus) || s_operator == toString(OMinusMinus);
	ExpressionPtr ep_value;
	if (!b_postfix) {
		ep_value = p_parser.p_expressionParser.parseExpression();
	}
	return { .target = t_target,
		 .op     = t_operator,
		 .value  = std::move(ep_value),
		 .span   = TokenSpan::spanThrough(t_target.getSpan(), p_parser.previous().getSpan()) };
}

}
