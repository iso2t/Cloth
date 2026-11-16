// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <parser/expr/expression.hpp>
#include <parser/parser.hpp>
#include <token/token.hpp>

namespace cloth {

#define CAST_PRECEDENCE       9
#define TYPE_CHECK_PRECEDENCE 7
#define POWER_PRECEDENCE      30

class ExpressionParser {
    public:
	ExpressionParser (Parser &parser) : p_parser(parser) {
	}

	enum Associativity { LEFT, RIGHT };

	struct OperatorPrecedence {
		int           precedence;
		Associativity associativity = Associativity::LEFT;
	};

    private:
	static const std::unordered_map<Operator, OperatorPrecedence> binaryOperators;

	Parser p_parser;

	Expression parseExpression (int minimumPrecedence = 0, bool stopAtAssignment = false, bool stopAtRange = false, bool allowWhileTrailer = false);
	Expression continueFromPrimary (Expression primary, bool allowWhileTrailer);
	Expression parseInfix (Expression initial, int minimumPrecedence, bool stopAtAssignment, bool stopAtRange = false, bool allowWhileTrailer = false);
	Expression parseArrowTarget (Expression receiver, Token op);
	Expression parseUnary (bool stopAtRange = false, bool allowWhileTrailer = false);
	Expression parsePostfix (Expression primary);
	Expression parseMember (Expression receiver);
	Expression parseDoubleColon (Expression receiver);
	Expression parsePrimary (bool allowWhileTrailer = false);
	Expression parseInterpolation ();

	std::vector<CallArgument> parseArgumentList ();
	std::vector<CallArgument> parseArguments ();
	std::string               decodeString (std::string contents);
	bool                      isImplicitBinding (Token token);
};

}
