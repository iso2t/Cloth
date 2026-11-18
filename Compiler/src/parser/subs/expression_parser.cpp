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

#include <ast/expr/class_expression.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/type/type_reference.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/if_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <syntax_errors.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>
#include <util/utf8.hpp>

namespace cloth {

namespace {

char decodeEscape (const char escaped) {
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
		throw cde::SyntaxErrors::expression(token);
	}
	return utf8_scalar_value(s_contents.substr(0, st_length));
}

std::string collapseInterpolationBraces (const std::string_view contents) {
	const char  c_open  = to_string(oBraceOpen).front();
	const char  c_close = to_string(oBraceClose).front();
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
	{ oElvis, { .precedence = 0, .associativity = RIGHT } },
	{ oOr, { .precedence = 1 } },
	{ oAnd, { .precedence = 2 } },
	{ oEqual, { .precedence = 5 } },
	{ oNotEqual, { .precedence = 5 } },
	{ oIdentity, { .precedence = 5 } },
	{ oLess, { .precedence = 7 } },
	{ oGreater, { .precedence = 7 } },
	{ oLessEqual, { .precedence = 7 } },
	{ oGreaterEqual, { .precedence = 7 } },
	{ oPlus, { .precedence = 10 } },
	{ oMinus, { .precedence = 10 } },
	{ oMul, { .precedence = 20 } },
	{ oDiv, { .precedence = 20 } },
	{ oMod, { .precedence = 20 } },
	{ oMulMul, { .precedence = POWER_PRECEDENCE, .associativity = RIGHT } },
};

ExpressionPtr ExpressionParser::parseExpression (const int minimumPrecedence, const bool stopAtAssignment, const bool stopAtRange,
                                                 const bool allowWhileTrailer) {
	return parseInfix(parseUnary(stopAtRange, allowWhileTrailer), minimumPrecedence, stopAtAssignment, stopAtRange, allowWhileTrailer);
}

ExpressionPtr ExpressionParser::continueFromPrimary (ExpressionPtr primary, const bool allowWhileTrailer) {
	return parseInfix(parsePostfix(std::move(primary)), 0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
}

ExpressionPtr ExpressionParser::parseInfix (ExpressionPtr initial, const int minimumPrecedence, const bool stopAtAssignment, const bool stopAtRange,
                                            const bool allowWhileTrailer) {
	ExpressionPtr ep_left = std::move(initial);
	while (true) {
		const Token        t_operator = p_parser.peek();
		const std::string &s_operator = t_operator.get_lexeme();
		if (stopAtAssignment && Parser::assignmentOperators.contains(s_operator)) {
			break;
		}
		if (t_operator.get_type() == TokenType::Keyword && s_operator == to_string(kIs)) {
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

		const Operator o_kind    = fromCharacter(s_operator).value_or(oError);
		const bool     b_isRange = o_kind == oRange || o_kind == oRangeInclusive;
		// The loop parser consumes an outer range delimiter.
		if ((stopAtRange && b_isRange) || o_kind == oParenClose || o_kind == oBraceClose || o_kind == oBracketClose || o_kind == oComma ||
		    o_kind == oSemicolon) {
			break;
		}
		if (o_kind == oPointerRight) {
			if (CAST_PRECEDENCE < minimumPrecedence) {
				break;
			}
			p_parser.advance();
			ep_left = parseArrowTarget(std::move(ep_left), t_operator);
			continue;
		}

		const auto it_precedence = binaryOperators.find(o_kind);
		if (!b_isRange && it_precedence == binaryOperators.end()) {
			throw cde::SyntaxErrors::unsupportedOperator(t_operator);
		}
		const auto [precedence, associativity] = b_isRange ? OperatorPrecedence{ .precedence = 8 } : it_precedence->second;
		if (precedence < minimumPrecedence) {
			break;
		}

		p_parser.advance();
		const int     i_rightPrecedence = associativity == LEFT ? precedence + 1 : precedence;
		ExpressionPtr ep_right          = parseExpression(i_rightPrecedence, /*stopAtAssignment=*/false, stopAtRange, allowWhileTrailer);
		if (o_kind == oElvis) {
			ep_left = std::make_shared<ElvisExpression>(std::move(ep_left), t_operator, std::move(ep_right));
		} else {
			ep_left = std::make_shared<BinaryExpression>(std::move(ep_left), t_operator, std::move(ep_right));
		}
	}
	return ep_left;
}

ExpressionPtr ExpressionParser::parseTypeCheck (ExpressionPtr operand, const Token &op, const bool stopAtRange, const bool allowWhileTrailer) {
	const Token        t_target          = p_parser.peek();
	const Token        t_next            = p_parser.peek(1);
	const std::string &s_next            = t_next.get_lexeme();
	const bool         b_hasValuePostfix = s_next == to_string(oParenOpen) || s_next == to_string(oDot) || s_next == to_string(oSafeDot) ||
	                               s_next == to_string(oColonColon) || s_next == to_string(oBracketOpen);
	const bool b_valueTarget = t_target.get_lexeme() == to_string(oParenOpen) || isImplicitBinding(t_target) ||
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
	if (p_parser.peek().get_type() != TokenType::Identifier || !at(oParenOpen, 1)) {
		return std::make_shared<CastExpression>(std::move(receiver), op, TypeParser(p_parser).parse(/*inCast=*/true));
	}
	const Token     t_target   = p_parser.advance();
	ExpressionPtr   ep_payload = p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr {
                p_parser.skipNewLines();
                if (at(oParenClose)) {
                        return nullptr;
                }
                ExpressionPtr ep_expression = parseExpression();
                p_parser.skipNewLines();
                return ep_expression;
        });
	const TokenSpan ts_span    = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
	return std::make_shared<StateTransitionExpression>(std::move(receiver), op, t_target, std::move(ep_payload), ts_span);
}

ExpressionPtr ExpressionParser::parseUnary (const bool stopAtRange, const bool allowWhileTrailer) {
	const Token        t_token  = p_parser.peek();
	const std::string &s_lexeme = t_token.get_lexeme();
	if (const bool b_not = s_lexeme == to_string(oNot);
	    t_token.get_type() == TokenType::Operator && (b_not || s_lexeme == to_string(oPlus) || s_lexeme == to_string(oMinus))) {
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
		if (s_lexeme == to_string(oBracketOpen)) {
			ExpressionPtr   ep_index = p_parser.p_delimitedParser.brackets([this] () -> ExpressionPtr { return parseExpression(); });
			const TokenSpan ts_span  = TokenSpan::span_through(ep_value->get_span(), p_parser.previous().get_span());
			ep_value                 = std::make_shared<IndexExpression>(std::move(ep_value), std::move(ep_index), ts_span);
		} else if (s_lexeme == to_string(oDot)) {
			p_parser.advance();
			ep_value = parseMember(std::move(ep_value));
		} else if (s_lexeme == to_string(oSafeDot)) {
			const Token                            t_operator      = p_parser.advance();
			std::shared_ptr<const GuardedReceiver> grp_payload     = std::make_shared<GuardedReceiver>(std::move(ep_value));
			ExpressionPtr                          ep_continuation = parsePostfix(parseMember(grp_payload));
			return std::make_shared<GuardedAccessExpression>(t_operator, std::move(grp_payload), std::move(ep_continuation));
		} else if (s_lexeme == to_string(oColonColon)) {
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
	if (!at(oParenOpen)) {
		return std::make_shared<FieldAccessExpression>(std::move(receiver), t_name);
	}
	std::vector<CallArgument> v_arguments = parseArgumentList();
	const TokenSpan           ts_span     = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
	return std::make_shared<MemberCallExpression>(std::move(receiver), t_name, std::move(v_arguments), ts_span, std::move(opt_typeArguments));
}

ExpressionPtr ExpressionParser::parseDoubleColon (ExpressionPtr receiver) {
	if (p_parser.atKeyword(kNew)) {
		const Token               t_keyword   = p_parser.advance();
		std::vector<CallArgument> v_arguments = at(oParenOpen) ? parseArgumentList() : std::vector<CallArgument>{};
		const TokenSpan           ts_span     = TokenSpan::span_through(receiver->get_span(), p_parser.previous().get_span());
		return std::make_shared<ClassCreationExpression>(std::move(receiver), t_keyword, std::move(v_arguments), ts_span);
	}
	if (p_parser.atKeyword(kDelete)) {
		return std::make_shared<ClassDeletionExpression>(std::move(receiver), p_parser.advance());
	}
	const bool                      b_genericCall     = p_parser.p_typeParser.startsCallTypeArguments();
	const Token                     t_name            = p_parser.expect(TokenType::Identifier, "a compiler data or state-method name");
	std::optional<TypeArgumentList> opt_typeArguments = b_genericCall ? std::make_optional(p_parser.p_typeParser.arguments()) : std::nullopt;
	if (!at(oParenOpen)) {
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
		if (s_lexeme == to_string(kIf)) {
			return std::make_shared<IfExpression>(p_parser.p_ifParser.parseIf(allowWhileTrailer));
		}
		if (s_lexeme == to_string(kNull)) {
			p_parser.advance();
			return std::make_shared<NullLiteral>(t_token.get_span());
		}
		if (const bool b_true = s_lexeme == to_string(kTrue); b_true || s_lexeme == to_string(kFalse)) {
			p_parser.advance();
			return std::make_shared<BooleanLiteral>(b_true, t_token.get_span());
		}
		if (isImplicitBinding(t_token)) {
			return std::make_shared<NameExpression>(p_parser.advance());
		}
		break;
	}
	case TokenType::Operator:
		if (s_lexeme == to_string(oBracketOpen)) {
			std::vector<ExpressionPtr> v_elements = p_parser.p_delimitedParser.brackets([this] () -> std::vector<ExpressionPtr> {
				return p_parser.p_delimitedParser.commaSeparated(oBracketClose, [this] () -> ExpressionPtr { return parseExpression(); });
			});
			return std::make_shared<ArrayLiteral>(std::move(v_elements),
			                                      TokenSpan::span_through(t_token.get_span(), p_parser.previous().get_span()));
		}
		if (s_lexeme == to_string(oBraceOpen)) {
			std::vector<ExpressionPtr> v_fields = p_parser.p_delimitedParser.braces([this] () -> std::vector<ExpressionPtr> {
				return p_parser.p_delimitedParser.commaSeparated(oBraceClose, [this] () -> ExpressionPtr { return parseExpression(); });
			});
			return std::make_shared<StructLiteral>(std::move(v_fields),
			                                       TokenSpan::span_through(t_token.get_span(), p_parser.previous().get_span()));
		}
		if (s_lexeme == to_string(oParenOpen)) {
			return p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr { return parseExpression(); });
		}
		break;
	case TokenType::InterpolationStart:
		return parseInterpolation();
	case TokenType::Identifier: {
		const bool                      b_genericCall     = p_parser.p_typeParser.startsCallTypeArguments();
		const Token                     t_name            = p_parser.advance();
		std::optional<TypeArgumentList> opt_typeArguments = b_genericCall ? std::make_optional(p_parser.p_typeParser.arguments()) : std::nullopt;
		if (!at(oParenOpen)) {
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
	throw cde::SyntaxErrors::expression(t_token);
}

std::vector<CallArgument> ExpressionParser::parseArgumentList () {
	return p_parser.p_delimitedParser.parentheses([this] () -> std::vector<CallArgument> { return parseArguments(); });
}

std::vector<CallArgument> ExpressionParser::parseArguments () {
	return p_parser.p_delimitedParser.commaSeparated(oParenClose, [this] () -> CallArgument {
		std::optional<Token> opt_label;
		std::optional<Token> opt_equals;
		if (p_parser.peek().get_type() == TokenType::Identifier && at(oAssign, 1)) {
			opt_label  = p_parser.advance();
			opt_equals = p_parser.advance();
		}
		return { .expression = parseExpression(), .label = std::move(opt_label), .equals = std::move(opt_equals) };
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
		p_parser.expect(TokenType::InterpolationOpen, std::format("'{}' before an interpolated expression", to_string(oBraceOpen)));
		p_parser.skipNewLines();
		v_parts.push_back(parseExpression());
		p_parser.skipNewLines();
		p_parser.expect(TokenType::InterpolationClose, std::format("'{}' after an interpolated expression", to_string(oBraceClose)));
	}
}

std::string ExpressionParser::decodeString (const std::string_view contents) {
	std::string s_result;
	s_result.reserve(contents.size());
	for (std::size_t st_index = 0; st_index < contents.size();) {
		const char c_character = contents[st_index++];
		s_result.push_back(c_character == '\\' ? decodeEscape(contents.at(st_index++)) : c_character);
	}
	return s_result;
}

bool ExpressionParser::isImplicitBinding (const Token &token) const {
	return token.get_type() == TokenType::Keyword &&
	       (token.get_lexeme() == to_string(kSelf) || (p_parser.b_inStateBody && token.get_lexeme() == to_string(kState)));
}

bool ExpressionParser::at (const Operator op, const int offset) const {
	return p_parser.peek(offset).get_lexeme() == to_string(op);
}

}
