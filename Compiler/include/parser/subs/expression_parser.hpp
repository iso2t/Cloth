// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
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

	ExpressionPtr parseExpression (int minimumPrecedence = 0, bool stopAtAssignment = false, bool stopAtRange = false, bool allowWhileTrailer = false);
	ExpressionPtr continueFromPrimary (ExpressionPtr primary, bool allowWhileTrailer);
	ExpressionPtr parseInfix (ExpressionPtr initial, int minimumPrecedence, bool stopAtAssignment, bool stopAtRange = false,
	                          bool allowWhileTrailer = false);
	ExpressionPtr parseArrowTarget (ExpressionPtr receiver, Token op);
	ExpressionPtr parseUnary (bool stopAtRange = false, bool allowWhileTrailer = false);
	ExpressionPtr parsePostfix (ExpressionPtr primary);
	ExpressionPtr parseMember (ExpressionPtr receiver);
	ExpressionPtr parseDoubleColon (ExpressionPtr receiver);
	ExpressionPtr parsePrimary (bool allowWhileTrailer = false);
	ExpressionPtr parseInterpolation ();

	std::vector<CallArgument> parseArgumentList ();
	std::vector<CallArgument> parseArguments ();
	std::string               decodeString (std::string contents);
	bool                      isImplicitBinding (Token token);
};

}
