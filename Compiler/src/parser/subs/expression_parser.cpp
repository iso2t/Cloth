// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <parser/expr/class_expression.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/expr/member_call_expression.hpp>
#include <parser/expr/provider_expressions.hpp>
#include <parser/expr/struct_expressions.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/if_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <parser/type/type_reference.hpp>
#include <token/token.hpp>
#include <util/utf8.hpp>

namespace cloth {

namespace {

char decodeEscape (char escaped) {
	switch (escaped) {
	case 'n':
		return '\n';
	case 'r':
		return '\r';
	case 't':
		return '\t';
	case '0':
		return '\0';
	default:
		return escaped; // The lexer validates the escaped quote or backslash.
	}
}

char32_t decodeCharacter (const Token &token) {
	const std::string_view s_lexeme   = token.get_lexeme();
	const std::string_view s_contents = s_lexeme.substr(1, s_lexeme.size() - 2);
	if (s_contents.starts_with('\\')) {
		return static_cast<unsigned char>(decodeEscape(s_contents.at(1)));
	}

	const std::size_t st_length = utf8_scalar_length(s_contents);
	if (st_length == 0) {
		throw SyntaxErrors::expression(token);
	}
	const unsigned char c_first = static_cast<unsigned char>(s_contents.front());
	if (st_length == 1) {
		return c_first;
	}
	char32_t c32_value = c_first & (0x7Fu >> st_length);
	for (std::size_t st_index = 1; st_index < st_length; ++st_index) {
		c32_value = (c32_value << 6) | (static_cast<unsigned char>(s_contents[st_index]) & 0x3Fu);
	}
	return c32_value;
}

std::string collapseInterpolationBraces (std::string_view contents) {
	const char  c_open  = cloth::to_string(Operator::o_BRACE_OPEN).front();
	const char  c_close = cloth::to_string(Operator::o_BRACE_CLOSE).front();
	std::string s_result;
	s_result.reserve(contents.size());
	for (std::size_t st_index = 0; st_index < contents.size();) {
		const char c_character = contents[st_index++];
		s_result.push_back(c_character);
		if ((c_character == c_open || c_character == c_close) && st_index < contents.size() && contents[st_index] == c_character) {
			++st_index;
		}
	}
	return s_result;
}

}

const std::unordered_map<Operator, ExpressionParser::OperatorPrecedence> ExpressionParser::binaryOperators = {
	{ Operator::o_ELVIS, { 0, Associativity::RIGHT } },
	{ Operator::o_OR, { 1 } },
	{ Operator::o_AND, { 2 } },
	{ Operator::o_EQUAL, { 5 } },
	{ Operator::o_NOT_EQUAL, { 5 } },
	{ Operator::o_IDENTITY, { 5 } },
	{ Operator::o_LESS, { 7 } },
	{ Operator::o_GREATER, { 7 } },
	{ Operator::o_LESS_EQUAL, { 7 } },
	{ Operator::o_GREATER_EQUAL, { 7 } },
	{ Operator::o_PLUS, { 10 } },
	{ Operator::o_MINUS, { 10 } },
	{ Operator::o_MULTIPLY, { 20 } },
	{ Operator::o_DIVIDE, { 20 } },
	{ Operator::o_MODULO, { 20 } },
	{ Operator::o_MUL_MUL, { POWER_PRECEDENCE, Associativity::RIGHT } },
};

ExpressionPtr ExpressionParser::parseExpression (int minimumPrecedence, bool stopAtAssignment, bool stopAtRange, bool allowWhileTrailer) {
	return parseInfix(parseUnary(stopAtRange, allowWhileTrailer), minimumPrecedence, stopAtAssignment, stopAtRange, allowWhileTrailer);
}

ExpressionPtr ExpressionParser::continueFromPrimary (ExpressionPtr primary, bool allowWhileTrailer) {
	return parseInfix(parsePostfix(std::move(primary)), 0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
}

ExpressionPtr ExpressionParser::parseInfix (ExpressionPtr initial, int minimumPrecedence, bool stopAtAssignment, bool stopAtRange, bool allowWhileTrailer) {
	ExpressionPtr ep_left = std::move(initial);
	while (true) {
		const Token        t_operator = p_parser.peek();
		const std::string &s_operator = t_operator.get_lexeme();
		if (stopAtAssignment && Parser::assignmentOperators.contains(s_operator)) {
			break;
		}
		if (t_operator.get_type() == TokenType::Keyword && s_operator == cloth::to_string(Keyword::k_IS)) {
			if (TYPE_CHECK_PRECEDENCE < minimumPrecedence) {
				break;
			}
			p_parser.advance();
			ep_left = parseTypeCheck(std::move(ep_left), t_operator, stopAtRange, allowWhileTrailer);
			continue;
		}
		if (t_operator.get_type() != TokenType::Operator) {
			break;
		}

		const Operator o_kind    = fromCharacter(s_operator).value_or(Operator::o_ERROR);
		const bool     b_isRange = o_kind == Operator::o_RANGE || o_kind == Operator::o_RANGE_INCLUSIVE;
		// The loop parser consumes an outer range delimiter.
		if ((stopAtRange && b_isRange) || o_kind == Operator::o_PAREN_CLOSE || o_kind == Operator::o_BRACE_CLOSE ||
		    o_kind == Operator::o_BRACKET_CLOSE || o_kind == Operator::o_COMMA || o_kind == Operator::o_SEMICOLON) {
			break;
		}
		if (o_kind == Operator::o_POINTER_RIGHT) {
			if (CAST_PRECEDENCE < minimumPrecedence) {
				break;
			}
			p_parser.advance();
			ep_left = parseArrowTarget(std::move(ep_left), t_operator);
			continue;
		}

		const std::unordered_map<Operator, OperatorPrecedence>::const_iterator it_precedence = binaryOperators.find(o_kind);
		if (!b_isRange && it_precedence == binaryOperators.end()) {
			throw SyntaxErrors::unsupportedOperator(t_operator);
		}
		const OperatorPrecedence op_precedence = b_isRange ? OperatorPrecedence{ 8 } : it_precedence->second;
		if (op_precedence.precedence < minimumPrecedence) {
			break;
		}

		p_parser.advance();
		const int     i_rightPrecedence = op_precedence.associativity == Associativity::LEFT ? op_precedence.precedence + 1 : op_precedence.precedence;
		ExpressionPtr ep_right          = parseExpression(i_rightPrecedence, /*stopAtAssignment=*/false, stopAtRange, allowWhileTrailer);
		if (o_kind == Operator::o_ELVIS) {
			ep_left = std::make_shared<ElvisExpression>(std::move(ep_left), t_operator, std::move(ep_right));
		} else {
			ep_left = std::make_shared<BinaryExpression>(std::move(ep_left), t_operator, std::move(ep_right));
		}
	}
	return ep_left;
}

ExpressionPtr ExpressionParser::parseTypeCheck (ExpressionPtr operand, const Token &op, bool stopAtRange, bool allowWhileTrailer) {
	const Token        t_target          = p_parser.peek();
	const Token        t_next            = p_parser.peek(1);
	const std::string &s_next            = t_next.get_lexeme();
	const bool         b_hasValuePostfix = s_next == cloth::to_string(Operator::o_PAREN_OPEN) || s_next == cloth::to_string(Operator::o_DOT) ||
	                                       s_next == cloth::to_string(Operator::o_SAFE_DOT) || s_next == cloth::to_string(Operator::o_COLON_COLON) ||
	                                       s_next == cloth::to_string(Operator::o_BRACKET_OPEN);
	const bool b_valueTarget = t_target.get_lexeme() == cloth::to_string(Operator::o_PAREN_OPEN) || isImplicitBinding(t_target) ||
	                           (t_target.get_type() == TokenType::Identifier && (b_hasValuePostfix || p_parser.p_typeParser.startsCallTypeArguments()));
	if (!b_valueTarget || p_parser.p_typeParser.startsQualifiedTypeCheck()) {
		return std::make_shared<TypeCheckExpression>(std::move(operand), op, p_parser.p_typeParser.parse(/*inCast=*/true));
	}

	ExpressionPtr ep_right = parseUnary(stopAtRange, allowWhileTrailer);
	if (std::shared_ptr<const FieldAccessExpression> fap_candidate = std::dynamic_pointer_cast<const FieldAccessExpression>(ep_right)) {
		const TokenSpan ts_target = t_target.get_span();
		// The parser and AST copy tokens. Match the source occurrence of the root.
		const std::optional<Token> opt_path = typeNameCandidate(*fap_candidate, [&t_target, ts_target] (const Token &root) -> bool {
			const TokenSpan ts_root = root.get_span();
			return root.get_type() == t_target.get_type() && root.get_lexeme() == t_target.get_lexeme() &&
			       ts_root.startLine == ts_target.startLine && ts_root.startCol == ts_target.startCol && ts_root.endLine == ts_target.endLine &&
			       ts_root.endCol == ts_target.endCol;
		});
		if (opt_path) {
			return std::make_shared<TypeCheckExpression>(std::move(operand), op, TypeReference(*opt_path), std::move(fap_candidate));
		}
	}
	return std::make_shared<BinaryExpression>(std::move(operand), op, std::move(ep_right));
}

ExpressionPtr ExpressionParser::parseArrowTarget (ExpressionPtr receiver, const Token &op) {
	if (p_parser.peek().get_type() != TokenType::Identifier || !at(Operator::o_PAREN_OPEN, 1)) {
		return std::make_shared<CastExpression>(std::move(receiver), op, TypeParser(p_parser).parse(/*inCast=*/true));
	}
	const Token     t_target   = p_parser.advance();
	ExpressionPtr   ep_payload = p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr {
		p_parser.skipNewLines();
		if (at(Operator::o_PAREN_CLOSE)) {
			return nullptr;
		}
		ExpressionPtr ep_expression = parseExpression();
		p_parser.skipNewLines();
		return ep_expression;
	});
	const TokenSpan ts_span    = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
	return std::make_shared<StateTransitionExpression>(std::move(receiver), op, t_target, std::move(ep_payload), ts_span);
}

ExpressionPtr ExpressionParser::parseUnary (bool stopAtRange, bool allowWhileTrailer) {
	const Token        t_token  = p_parser.peek();
	const std::string &s_lexeme = t_token.get_lexeme();
	const bool         b_not    = s_lexeme == cloth::to_string(Operator::o_NOT);
	if (t_token.get_type() == TokenType::Operator &&
	    (b_not || s_lexeme == cloth::to_string(Operator::o_PLUS) || s_lexeme == cloth::to_string(Operator::o_MINUS))) {
		p_parser.advance();
		ExpressionPtr ep_operand = b_not ? parseUnary(stopAtRange, allowWhileTrailer) :
		                                   parseExpression(POWER_PRECEDENCE, /*stopAtAssignment=*/false, stopAtRange, allowWhileTrailer);
		return std::make_shared<UnaryExpression>(t_token, std::move(ep_operand));
	}
	return parsePostfix(parsePrimary(allowWhileTrailer));
}

ExpressionPtr ExpressionParser::parsePostfix (ExpressionPtr primary) {
	ExpressionPtr ep_value = std::move(primary);
	while (true) {
		const Token        t_next   = p_parser.peek();
		const std::string &s_lexeme = t_next.get_lexeme();
		if (s_lexeme == cloth::to_string(Operator::o_BRACKET_OPEN)) {
			ExpressionPtr   ep_index = p_parser.p_delimitedParser.brackets([this] () -> ExpressionPtr { return parseExpression(); });
			const TokenSpan ts_span  = TokenSpan::span_through(ep_value->get_span(), p_parser.previous().get_span());
			ep_value                 = std::make_shared<IndexExpression>(std::move(ep_value), std::move(ep_index), ts_span);
		} else if (s_lexeme == cloth::to_string(Operator::o_DOT)) {
			p_parser.advance();
			ep_value = parseMember(std::move(ep_value));
		} else if (s_lexeme == cloth::to_string(Operator::o_SAFE_DOT)) {
			const Token                            t_operator      = p_parser.advance();
			std::shared_ptr<const GuardedReceiver> grp_payload     = std::make_shared<GuardedReceiver>(std::move(ep_value));
			ExpressionPtr                          ep_continuation = parsePostfix(parseMember(grp_payload));
			return std::make_shared<GuardedAccessExpression>(t_operator, std::move(grp_payload), std::move(ep_continuation));
		} else if (s_lexeme == cloth::to_string(Operator::o_COLON_COLON)) {
			p_parser.advance();
			ep_value = parseDoubleColon(std::move(ep_value));
		} else {
			return ep_value;
		}
	}
}

ExpressionPtr ExpressionParser::parseMember (ExpressionPtr receiver) {
	const bool                      b_genericCall     = p_parser.p_typeParser.startsCallTypeArguments();
	const Token                     t_name            = p_parser.expect(TokenType::Identifier, "a member name");
	std::optional<TypeArgumentList> opt_typeArguments = b_genericCall ? std::make_optional(p_parser.p_typeParser.arguments()) : std::nullopt;
	if (!at(Operator::o_PAREN_OPEN)) {
		return std::make_shared<FieldAccessExpression>(std::move(receiver), t_name);
	}
	std::vector<CallArgument> v_arguments = parseArgumentList();
	const TokenSpan           ts_span     = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
	return std::make_shared<MemberCallExpression>(std::move(receiver), t_name, std::move(v_arguments), ts_span, std::move(opt_typeArguments));
}

ExpressionPtr ExpressionParser::parseDoubleColon (ExpressionPtr receiver) {
	if (p_parser.atKeyword(Keyword::k_NEW)) {
		const Token               t_keyword   = p_parser.advance();
		std::vector<CallArgument> v_arguments = at(Operator::o_PAREN_OPEN) ? parseArgumentList() : std::vector<CallArgument>{};
		const TokenSpan           ts_span     = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
		return std::make_shared<ClassCreationExpression>(std::move(receiver), t_keyword, std::move(v_arguments), ts_span);
	}
	if (p_parser.atKeyword(Keyword::k_DELETE)) {
		return std::make_shared<ClassDeletionExpression>(std::move(receiver), p_parser.advance());
	}
	const bool                      b_genericCall     = p_parser.p_typeParser.startsCallTypeArguments();
	const Token                     t_name            = p_parser.expect(TokenType::Identifier, "a compiler data or state-method name");
	std::optional<TypeArgumentList> opt_typeArguments = b_genericCall ? std::make_optional(p_parser.p_typeParser.arguments()) : std::nullopt;
	if (!at(Operator::o_PAREN_OPEN)) {
		return std::make_shared<ProviderAccessExpression>(std::move(receiver), t_name);
	}
	std::vector<CallArgument> v_arguments = parseArgumentList();
	const TokenSpan           ts_span     = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
	return std::make_shared<StateCallExpression>(std::move(receiver), t_name, std::move(v_arguments), ts_span, std::move(opt_typeArguments));
}

ExpressionPtr ExpressionParser::parsePrimary (bool allowWhileTrailer) {
	const Token t_token = p_parser.peek();
	if (p_parser.p_typeParser.startsNamedTypeReceiver()) {
		return std::make_shared<TypeReceiverExpression>(p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
	}

	const std::string &s_lexeme = t_token.get_lexeme();
	switch (t_token.get_type()) {
	case TokenType::Keyword: {
		if (TypeParser::startsTypeReceiver(t_token)) {
			return std::make_shared<TypeReceiverExpression>(TypeParser(p_parser).parse(/*inCast=*/false, /*allowArrays=*/true));
		}
		if (s_lexeme == cloth::to_string(Keyword::k_IF)) {
			return std::make_shared<IfExpression>(p_parser.p_ifParser.parseIf(allowWhileTrailer));
		}
		if (s_lexeme == cloth::to_string(Keyword::k_NULL)) {
			p_parser.advance();
			return std::make_shared<NullLiteral>(t_token.get_span());
		}
		const bool b_true = s_lexeme == cloth::to_string(Keyword::k_TRUE);
		if (b_true || s_lexeme == cloth::to_string(Keyword::k_FALSE)) {
			p_parser.advance();
			return std::make_shared<BooleanLiteral>(b_true, t_token.get_span());
		}
		if (isImplicitBinding(t_token)) {
			return std::make_shared<NameExpression>(p_parser.advance());
		}
		break;
	}
	case TokenType::Operator:
		if (s_lexeme == cloth::to_string(Operator::o_BRACKET_OPEN)) {
			std::vector<ExpressionPtr> v_elements = p_parser.p_delimitedParser.brackets([this] () -> std::vector<ExpressionPtr> {
				return p_parser.p_delimitedParser.commaSeparated(Operator::o_BRACKET_CLOSE,
				                                                 [this] () -> ExpressionPtr { return parseExpression(); });
			});
			return std::make_shared<ArrayLiteral>(std::move(v_elements),
			                                      TokenSpan::span_through(t_token.get_span(), p_parser.previous().get_span()));
		}
		if (s_lexeme == cloth::to_string(Operator::o_BRACE_OPEN)) {
			std::vector<ExpressionPtr> v_fields = p_parser.p_delimitedParser.braces([this] () -> std::vector<ExpressionPtr> {
				return p_parser.p_delimitedParser.commaSeparated(Operator::o_BRACE_CLOSE,
				                                                 [this] () -> ExpressionPtr { return parseExpression(); });
			});
			return std::make_shared<StructLiteral>(std::move(v_fields),
			                                       TokenSpan::span_through(t_token.get_span(), p_parser.previous().get_span()));
		}
		if (s_lexeme == cloth::to_string(Operator::o_PAREN_OPEN)) {
			return p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr { return parseExpression(); });
		}
		break;
	case TokenType::InterpolationStart:
		return parseInterpolation();
	case TokenType::Identifier: {
		const bool                      b_genericCall     = p_parser.p_typeParser.startsCallTypeArguments();
		const Token                     t_name            = p_parser.advance();
		std::optional<TypeArgumentList> opt_typeArguments = b_genericCall ? std::make_optional(p_parser.p_typeParser.arguments()) : std::nullopt;
		if (!at(Operator::o_PAREN_OPEN)) {
			return std::make_shared<NameExpression>(t_name);
		}
		std::vector<CallArgument> v_arguments = parseArgumentList();
		const TokenSpan           ts_span     = TokenSpan::span_through(t_name.get_span(), p_parser.previous().get_span());
		return std::make_shared<CallExpression>(t_name, std::move(v_arguments), ts_span, std::move(opt_typeArguments));
	}
	case TokenType::StringLiteral:
	case TokenType::RawStringLiteral: {
		p_parser.advance();
		const bool             b_raw      = t_token.get_type() == TokenType::RawStringLiteral;
		const std::size_t      st_quotes  = b_raw ? 3 : 1;
		const std::string_view s_contents = std::string_view(s_lexeme).substr(st_quotes, s_lexeme.size() - 2 * st_quotes);
		return std::make_shared<StringLiteral>(b_raw ? std::string(s_contents) : decodeString(s_contents), t_token.get_span());
	}
	case TokenType::CharacterLiteral:
		p_parser.advance();
		return std::make_shared<CharacterLiteral>(decodeCharacter(t_token), t_token.get_span());
	case TokenType::IntegerLiteral:
		p_parser.advance();
		return std::make_shared<IntegerLiteral>(integerLiteralValue(t_token), t_token);
	case TokenType::FloatingPointLiteral:
		p_parser.advance();
		return std::make_shared<FloatingPointLiteral>(floatingLiteralText(t_token), t_token);
	default:
		break;
	}
	throw SyntaxErrors::expression(t_token);
}

std::vector<CallArgument> ExpressionParser::parseArgumentList () {
	return p_parser.p_delimitedParser.parentheses([this] () -> std::vector<CallArgument> { return parseArguments(); });
}

std::vector<CallArgument> ExpressionParser::parseArguments () {
	return p_parser.p_delimitedParser.commaSeparated(Operator::o_PAREN_CLOSE, [this] () -> CallArgument {
		std::optional<Token> opt_label;
		std::optional<Token> opt_equals;
		if (p_parser.peek().get_type() == TokenType::Identifier && at(Operator::o_ASSIGN, 1)) {
			opt_label  = p_parser.advance();
			opt_equals = p_parser.advance();
		}
		return { parseExpression(), std::move(opt_label), std::move(opt_equals) };
	});
}

ExpressionPtr ExpressionParser::parseInterpolation () {
	const Token                t_opening = p_parser.advance();
	std::vector<ExpressionPtr> v_parts;
	while (true) {
		const Token t_segment = p_parser.expect(TokenType::StringSegment, "string text");
		v_parts.push_back(std::make_shared<StringLiteral>(decodeString(collapseInterpolationBraces(t_segment.get_lexeme())), t_segment.get_span()));
		if (p_parser.peek().get_type() == TokenType::InterpolationEnd) {
			const Token t_closing = p_parser.advance();
			return std::make_shared<InterpolatedString>(std::move(v_parts), TokenSpan::span_through(t_opening.get_span(), t_closing.get_span()));
		}
		p_parser.expect(TokenType::InterpolationOpen, std::format("'{}' before an interpolated expression", cloth::to_string(Operator::o_BRACE_OPEN)));
		p_parser.skipNewLines();
		v_parts.push_back(parseExpression());
		p_parser.skipNewLines();
		p_parser.expect(TokenType::InterpolationClose, std::format("'{}' after an interpolated expression", cloth::to_string(Operator::o_BRACE_CLOSE)));
	}
}

std::string ExpressionParser::decodeString (std::string_view contents) {
	std::string s_result;
	s_result.reserve(contents.size());
	for (std::size_t st_index = 0; st_index < contents.size();) {
		const char c_character = contents[st_index++];
		s_result.push_back(c_character == '\\' ? decodeEscape(contents.at(st_index++)) : c_character);
	}
	return s_result;
}

bool ExpressionParser::isImplicitBinding (const Token &token) const {
	return token.get_type() == TokenType::Keyword && (token.get_lexeme() == cloth::to_string(Keyword::k_SELF) ||
	                                                  (p_parser.b_inStateBody && token.get_lexeme() == cloth::to_string(Keyword::k_STATE)));
}

bool ExpressionParser::at (Operator op, int offset) {
	return p_parser.peek(offset).get_lexeme() == cloth::to_string(op);
}

}
