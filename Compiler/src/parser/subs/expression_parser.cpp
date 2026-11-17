// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdexcept>
#include <utility>

#include <parser/expr/expression.hpp>
#include <parser/subs/expression_parser.hpp>
#include <token/token.hpp>

namespace cloth {

namespace {
// TODO: end goal is for this to no longer be needed, as we are still implementing, this is a requirement
#define NOT_IMPLEMENTED throw std::logic_error("Not Implemented!");
}

inline const std::unordered_map<Operator, ExpressionParser::OperatorPrecedence> ExpressionParser::binaryOperators = {
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
	return parseInfix(parsePostfix(std::move(primary)), 0, false, false, allowWhileTrailer);
}

ExpressionPtr ExpressionParser::parseInfix (ExpressionPtr initial, int minimumPrecedence, bool stopAtAssignment, bool stopAtRange,
                                            bool allowWhileTrailer){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parseArrowTarget(ExpressionPtr receiver, Token op){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parseUnary(bool stopAtRange, bool allowWhileTrailer){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parsePostfix(ExpressionPtr primary){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parseMember(ExpressionPtr receiver){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parseDoubleColon(ExpressionPtr receiver){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parsePrimary(bool allowWhileTrailer){ NOT_IMPLEMENTED }

ExpressionPtr ExpressionParser::parseInterpolation(){ NOT_IMPLEMENTED }

std::vector<CallArgument> ExpressionParser::parseArgumentList() {
	return parseArguments();
}

std::vector<CallArgument> ExpressionParser::parseArguments (){ NOT_IMPLEMENTED }

std::string ExpressionParser::decodeString(std::string contents) {
	std::string s_result;
	s_result.reserve(contents.size());

	std::size_t st_index = 0;
	while (st_index < contents.size()) {
		char c_character = contents[st_index++];
		if (c_character != '\\')
			s_result.push_back(c_character);
		else {
			char c_escaped = contents[st_index++];
			switch (c_escaped) {
			case 'n':
				s_result.push_back('\n');
				break;
			case 'r':
				s_result.push_back('\r');
				break;
			case 't':
				s_result.push_back('\t');
				break;
			case '0':
				s_result.push_back('\0');
				break;
			default:
				s_result.push_back(c_escaped); // validated by lexer
			}
		}
	}

	return s_result;
}

bool ExpressionParser::isImplicitBinding (Token token) {
	return token.get_type() == TokenType::Keyword &&
	       (token.get_lexeme() == cloth::to_string(Keyword::k_SELF) || p_parser.b_inStateBody && token.get_lexeme() == cloth::to_string(Keyword::k_STATE))
}

}
