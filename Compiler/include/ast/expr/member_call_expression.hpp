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

struct MemberCallExpression : CallSite {
	ExpressionPtr                   receiver;
	TokenSpan                       span;
	std::optional<TypeArgumentList> typeArguments;

	MemberCallExpression (ExpressionPtr receiver, const Token &name, std::vector<CallArgument> suppliedArguments, TokenSpan span,
	                      std::optional<TypeArgumentList> typeArguments = std::nullopt) :
	        CallSite(name, std::move(suppliedArguments)),
	        receiver(std::move(receiver)),
	        span(span),
	        typeArguments(std::move(typeArguments)) {
	}

	TokenSpan get_span () const override {
		return span;
	}
};

}
