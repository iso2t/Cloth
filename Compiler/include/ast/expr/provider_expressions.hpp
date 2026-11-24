// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct TypeReceiverExpression : Expression {
	TypeReference annotation;

	explicit TypeReceiverExpression (TypeReference annotation) : annotation(std::move(annotation)) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return annotation.getSpan();
	}
};

struct ProviderAccessExpression : Expression {
	ExpressionPtr receiver;
	Token         name;

	ProviderAccessExpression (ExpressionPtr receiver, const Token &name) : receiver(std::move(receiver)), name(name) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(receiver->getSpan(), name.getSpan());
	}
};

struct StateCallExpression : CallSite {
	ExpressionPtr                   receiver;
	TokenSpan                       span;
	std::optional<TypeArgumentList> typeArguments;

	StateCallExpression (ExpressionPtr receiver, const Token &name, std::vector<CallArgument> suppliedArguments, TokenSpan span,
	                     std::optional<TypeArgumentList> typeArguments = std::nullopt) :
	        CallSite(name, std::move(suppliedArguments)),
	        receiver(std::move(receiver)),
	        span(std::move(span)),
	        typeArguments(std::move(typeArguments)) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

}
