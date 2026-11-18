// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <ast/expr/expression.hpp>
#include <token/token.hpp>

namespace cloth {

struct StructLiteral : Expression {
	std::vector<ExpressionPtr> fields;
	TokenSpan                  span;

	StructLiteral (std::vector<ExpressionPtr> fields, TokenSpan span) : fields(std::move(fields)), span(span) {
	}

	TokenSpan get_span () const override {
		return span;
	}
};

struct FieldAccessExpression : Expression {
	ExpressionPtr receiver;
	Token         name;

	FieldAccessExpression (ExpressionPtr receiver, const Token &name) : receiver(std::move(receiver)), name(name) {
	}

	TokenSpan get_span () const override {
		return TokenSpan::span_through(receiver->get_span(), name.get_span());
	}
};

std::optional<Token> typeNameCandidate (const Expression &expression, const std::function<bool(const Token &)> &acceptRoot);
bool                 hasFieldTarget (const Expression &expression);

}
