// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class ExpressionParser {
    public:
	explicit ExpressionParser (Parser &parser) : p_parser(parser) {
	}

	enum Associativity { LEFT, RIGHT };

	struct OperatorPrecedence {
		int           precedence;
		Associativity associativity = LEFT;
	};

	static const std::unordered_map<Operator, OperatorPrecedence> binaryOperators;

	ExpressionPtr parseExpression (int minimumPrecedence = 0, bool stopAtAssignment = false, bool stopAtRange = false, bool allowWhileTrailer = false);
	ExpressionPtr continueFromPrimary (ExpressionPtr primary, bool allowWhileTrailer);
	ExpressionPtr parseUnary (bool stopAtRange = false, bool allowWhileTrailer = false);
	ExpressionPtr parsePrimary (bool allowWhileTrailer = false);
	std::vector<CallArgument> parseArgumentList ();

    private:
	static constexpr int CAST_PRECEDENCE       = 9;
	static constexpr int TYPE_CHECK_PRECEDENCE = 7;
	static constexpr int POWER_PRECEDENCE      = 30;

	Parser &p_parser;

	ExpressionPtr             parseInfix (ExpressionPtr initial, int minimumPrecedence, bool stopAtAssignment, bool stopAtRange, bool allowWhileTrailer);
	ExpressionPtr             parseTypeCheck (ExpressionPtr operand, const Token &op, bool stopAtRange, bool allowWhileTrailer);
	ExpressionPtr             parseArrowTarget (ExpressionPtr receiver, const Token &op);
	ExpressionPtr             parsePostfix (ExpressionPtr primary);
	ExpressionPtr             parseMember (ExpressionPtr receiver);
	ExpressionPtr             parseDoubleColon (ExpressionPtr receiver);
	ExpressionPtr             parseInterpolation ();
	std::vector<CallArgument> parseArguments ();
	static std::string        decodeString (std::string_view contents);
	[[nodiscard]] bool        isImplicitBinding (const Token &token) const;
	bool                      at (Operator op, int offset = 0) const;
};

}
