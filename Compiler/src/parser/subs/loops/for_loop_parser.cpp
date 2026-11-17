// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <string>
#include <utility>

#include <parser/expr/expression.hpp>
#include <parser/parser.hpp>
#include <parser/stmt/for_statement.hpp>
#include <parser/stmt/loop_control.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/loops/for_loop_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <parser/type/type_reference.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

ForStatement ForLoopParser::parseFor (bool allowWhileTrailer) {
	const Token t_keyword = p_parser.advance();
	p_parser.skipNewLines();
	const Token t_opening = p_parser.expect(Operator::o_PAREN_OPEN);
	p_parser.skipNewLines();
	if (!p_parser.atKeyword(Keyword::k_DEF)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' before a loop binding", cloth::to_string(Keyword::k_DEF)), p_parser.peek());
	}
	const Token t_definition = p_parser.advance();
	p_parser.skipNewLines();

	std::optional<TypeReference> opt_type;
	std::optional<TokenSpan>     opt_annotationSpan;
	const Token                  t_annotationOpening = p_parser.peek();
	if (t_annotationOpening.get_lexeme() == cloth::to_string(Operator::o_LESS)) {
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
	ForBinding  fb_binding{ t_definition, std::move(opt_type), t_name, opt_annotationSpan };
	p_parser.skipNewLines();
	const Token t_arrow = p_parser.expect(Operator::o_POINTER_LEFT);
	p_parser.skipNewLines();

	const TokenSpan ts_sourceStart = p_parser.peek().get_span();
	ExpressionPtr   ep_first       = p_parser.p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/true);
	const TokenSpan ts_firstSpan   = TokenSpan::span_through(ts_sourceStart, p_parser.previous().get_span());
	p_parser.skipNewLines();

	const Token        t_afterFirst = p_parser.peek();
	const std::string &s_afterFirst = t_afterFirst.get_lexeme();
	const bool         b_isRange    = s_afterFirst == cloth::to_string(Operator::o_RANGE) || s_afterFirst == cloth::to_string(Operator::o_RANGE_INCLUSIVE);
	ForSource          fs_source{ ForSource::Elements{ ep_first, ts_firstSpan } };
	if (b_isRange) {
		const Token t_delimiter = p_parser.advance();
		p_parser.skipNewLines();
		ExpressionPtr ep_end = p_parser.p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/true);
		fs_source.value      = ForSource::Range{ std::move(ep_first), t_delimiter, std::move(ep_end),
			                                 TokenSpan::span_through(ts_sourceStart, p_parser.previous().get_span()) };
	}

	p_parser.skipNewLines();
	std::optional<Token> opt_separator = p_parser.isSemiColon(p_parser.peek()) ? std::make_optional(p_parser.advance()) : std::nullopt;
	if (opt_separator && !b_isRange) {
		throw cde::SyntaxErrors::expected(std::format("'{}' after a foreach source; steppers are only allowed on numeric ranges",
		                                         cloth::to_string(Operator::o_PAREN_CLOSE)),
		                             *opt_separator);
	}
	std::optional<ForStepper> opt_stepper = opt_separator ? std::make_optional(parseForStepper(t_name)) : std::nullopt;
	p_parser.skipNewLines();
	if (b_isRange && !opt_separator && p_parser.peek().get_lexeme() != cloth::to_string(Operator::o_PAREN_CLOSE)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' before a range stepper or '{}' after the range", cloth::to_string(Operator::o_SEMICOLON),
		                                         cloth::to_string(Operator::o_PAREN_CLOSE)),
		                             p_parser.peek());
	}
	const Token t_closing = p_parser.expect(Operator::o_PAREN_CLOSE);
	LoopBody    lb_body   = parseLoopBody(t_keyword, allowWhileTrailer);
	return { t_keyword, t_opening,         std::move(fb_binding), t_arrow, std::move(fs_source), std::move(opt_separator), std::move(opt_stepper),
		 t_closing, std::move(lb_body) };
}

ForStepper ForLoopParser::parseForStepper (const Token &counter) {
	p_parser.skipNewLines();
	const std::string s_description = std::format("the loop counter '{}' as the stepper target", counter.get_lexeme());
	const Token       t_target      = p_parser.expect(TokenType::Identifier, s_description);
	if (t_target.get_lexeme() != counter.get_lexeme()) {
		throw cde::SyntaxErrors::expected(s_description, t_target);
	}

	p_parser.skipNewLines();
	const Token        t_operator = p_parser.peek();
	const std::string &s_operator = t_operator.get_lexeme();
	if (t_operator.get_type() != TokenType::Operator || !Parser::assignmentOperators.contains(s_operator) ||
	    s_operator == cloth::to_string(Operator::o_FINAL_ASSIGN)) {
		throw cde::SyntaxErrors::expected(std::format("a counter assignment/update operator ({}, {}, {}, {}, {}, {}, {} or {})",
		                                         cloth::to_string(Operator::o_ASSIGN), cloth::to_string(Operator::o_PLUS_EQUAL),
		                                         cloth::to_string(Operator::o_MINUS_EQUAL), cloth::to_string(Operator::o_MUL_EQUAL),
		                                         cloth::to_string(Operator::o_DIV_EQUAL), cloth::to_string(Operator::o_MOD_EQUAL),
		                                         cloth::to_string(Operator::o_PLUS_PLUS), cloth::to_string(Operator::o_MINUS_MINUS)),
		                             t_operator);
	}
	p_parser.advance();
	const bool    b_postfix = s_operator == cloth::to_string(Operator::o_PLUS_PLUS) || s_operator == cloth::to_string(Operator::o_MINUS_MINUS);
	ExpressionPtr ep_value;
	if (!b_postfix) {
		p_parser.skipNewLines();
		ep_value = p_parser.p_expressionParser.parseExpression();
	}
	return { t_target, t_operator, std::move(ep_value), TokenSpan::span_through(t_target.get_span(), p_parser.previous().get_span()) };
}

}
