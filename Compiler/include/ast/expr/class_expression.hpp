// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <utility>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct ClassCreationExpression : CallSite {
	ExpressionPtr receiver;
	TokenSpan     span;

	ClassCreationExpression (ExpressionPtr receiver, const Token &name, std::vector<CallArgument> suppliedArguments, const TokenSpan &span) :
	        CallSite(name, std::move(suppliedArguments)),
	        receiver(std::move(receiver)),
	        span(span) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct ClassDeletionExpression : Expression {
	ExpressionPtr receiver;
	Token         keyword;

	ClassDeletionExpression (ExpressionPtr receiver, const Token &keyword) : receiver(std::move(receiver)), keyword(keyword) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(receiver->getSpan(), keyword.getSpan());
	}
};

/** The span includes the parenthesized payload position, even when it is empty. */
struct StateTransitionExpression : Expression {
	ExpressionPtr receiver;
	Token         op;
	Token         target;
	ExpressionPtr payload;
	TokenSpan     span;

	StateTransitionExpression (ExpressionPtr receiver, const Token &op, const Token &target, ExpressionPtr payload, TokenSpan span) :
	        receiver(std::move(receiver)),
	        op(op),
	        target(target),
	        payload(std::move(payload)),
	        span(std::move(span)) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct ConstructorDelegation : CallSite {
	TokenSpan     span;
	TypeReference type;

	ConstructorDelegation (const Token &name, std::vector<CallArgument> suppliedArguments, TokenSpan span, TypeReference type) :
	        CallSite(name, std::move(suppliedArguments)),
	        span(std::move(span)),
	        type(std::move(type)) {
	}

	ConstructorDelegation (const Token &name, std::vector<CallArgument> suppliedArguments, const TokenSpan &span) :
	        ConstructorDelegation(name, std::move(suppliedArguments), span, TypeReference(name)) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

}
