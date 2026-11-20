// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <string>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/type/type_reference.hpp>
#include <cde/syntax_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/loops/for_loop_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {

ForStatement ForLoopParser::parseFor (bool allowWhileTrailer) {
	const Token t_keyword = p_parser.advance();
	p_parser.skipNewLines();
	const Token t_opening = p_parser.expect(oParenOpen);
	p_parser.skipNewLines();
	if (!p_parser.atKeyword(kDef)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' before a loop binding", to_string(kDef)), p_parser.peek());
	}
	const Token t_definition = p_parser.advance();
	p_parser.skipNewLines();

	std::optional<TypeReference> opt_type;
	std::optional<TokenSpan>     opt_annotationSpan;
	if (const Token t_annotationOpening = p_parser.peek(); t_annotationOpening.get_lexeme() == to_string(oLess)) {
		opt_type           = get_delimited().angles([this] () -> TypeReference {
                        p_parser.skipNewLines();
                        TypeReference tr_type = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
                        p_parser.skipNewLines();
                        return tr_type;
                });
		opt_annotationSpan = TokenSpan::span_through(t_annotationOpening.get_span(), p_parser.previous().get_span());
	}

	p_parser.skipNewLines();
	const Token t_name = p_parser.expect(TokenType::Identifier, "a loop binding name");
	ForBinding  fb_binding{ .keyword = t_definition, .type = std::move(opt_type), .name = t_name, .annotationSpan = opt_annotationSpan };
	p_parser.skipNewLines();
	const Token t_arrow = p_parser.expect(oPointerLeft);
	p_parser.skipNewLines();

	const TokenSpan ts_sourceStart = p_parser.peek().get_span();
	ExpressionPtr   ep_first       = p_parser.p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/true);
	const TokenSpan ts_firstSpan   = TokenSpan::span_through(ts_sourceStart, p_parser.previous().get_span());
	p_parser.skipNewLines();

	const Token        t_afterFirst = p_parser.peek();
	const std::string &s_afterFirst = t_afterFirst.get_lexeme();
	const bool         b_isRange    = s_afterFirst == to_string(oRange) || s_afterFirst == to_string(oRangeInclusive);
	ForSource          fs_source{ ForSource::Elements{ .expression = ep_first, .span = ts_firstSpan } };
	if (b_isRange) {
		const Token t_delimiter = p_parser.advance();
		p_parser.skipNewLines();
		ExpressionPtr ep_end = p_parser.p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/true);
		fs_source.value      = ForSource::Range{ .expression = std::make_shared<BinaryExpression>(std::move(ep_first), t_delimiter, std::move(ep_end)),
			                                 .span       = TokenSpan::span_through(ts_sourceStart, p_parser.previous().get_span()) };
	}

	p_parser.skipNewLines();
	std::optional<Token> opt_separator = p_parser.isSemiColon(p_parser.peek()) ? std::make_optional(p_parser.advance()) : std::nullopt;
	if (opt_separator && !b_isRange) {
		throw cde::SyntaxErrors::expected(
		        std::format("'{}' after a foreach source; steppers are only allowed on numeric ranges", to_string(oParenClose)), *opt_separator);
	}
	std::optional<ForStepper> opt_stepper = opt_separator ? std::make_optional(parseForStepper(t_name)) : std::nullopt;
	p_parser.skipNewLines();
	if (b_isRange && !opt_separator && p_parser.peek().get_lexeme() != to_string(oParenClose)) {
		throw cde::SyntaxErrors::expected(
		        std::format("'{}' before a range stepper or '{}' after the range", to_string(oSemicolon), to_string(oParenClose)), p_parser.peek());
	}
	const Token t_closing = p_parser.expect(oParenClose);
	LoopBody    lb_body   = parseLoopBody(t_keyword, allowWhileTrailer);
	return { t_keyword, t_opening,         std::move(fb_binding), t_arrow, std::move(fs_source), std::move(opt_separator), std::move(opt_stepper),
		 t_closing, std::move(lb_body) };
}

ForStepper ForLoopParser::parseForStepper (const Token &counter) const {
	p_parser.skipNewLines();
	const std::string s_description = std::format("the loop counter '{}' as the stepper target", counter.get_lexeme());
	const Token       t_target      = p_parser.expect(TokenType::Identifier, s_description);
	if (t_target.get_name_key() != counter.get_name_key()) {
		throw cde::SyntaxErrors::expected(s_description, t_target);
	}

	p_parser.skipNewLines();
	const Token        t_operator = p_parser.peek();
	const std::string &s_operator = t_operator.get_lexeme();
	if (t_operator.get_type() != TokenType::Operator || !Parser::assignmentOperators.contains(s_operator) || s_operator == to_string(oFinalAssign)) {
		throw cde::SyntaxErrors::expected(std::format("a counter assignment/update operator ({}, {}, {}, {}, {}, {}, {} or {})", to_string(oAssign),
		                                              to_string(oPlusEqual), to_string(oMinusEqual), to_string(oMulEqual), to_string(oDivEqual),
		                                              to_string(oModEqual), to_string(oPlusPlus), to_string(oMinusMinus)),
		                                  t_operator);
	}
	p_parser.advance();
	const bool    b_postfix = s_operator == to_string(oPlusPlus) || s_operator == to_string(oMinusMinus);
	ExpressionPtr ep_value;
	if (!b_postfix) {
		p_parser.skipNewLines();
		ep_value = p_parser.p_expressionParser.parseExpression();
	}
	return { .target = t_target,
		 .op     = t_operator,
		 .value  = std::move(ep_value),
		 .span   = TokenSpan::span_through(t_target.get_span(), p_parser.previous().get_span()) };
}

}
