// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/stmt/loop_control.hpp>
#include <token/token.hpp>

namespace cloth {

struct DoWhileStatement : LoopStatement {
	Token         keyword;
	LoopBody      body;
	Token         whileKeyword;
	Token         opening;
	ExpressionPtr condition;
	Token         closing;
	LoopTargetPtr loopTarget = std::make_shared<LoopTarget>();

	DoWhileStatement (const Token &keyword, LoopBody body, const Token &whileKeyword, const Token &opening, ExpressionPtr condition, const Token &closing) :
	        keyword(keyword),
	        body(std::move(body)),
	        whileKeyword(whileKeyword),
	        opening(opening),
	        condition(std::move(condition)),
	        closing(closing) {
	}

	[[nodiscard]] const LoopTargetPtr &getLoopTarget () const override {
		return loopTarget;
	}

	[[nodiscard]] TokenSpan getConditionSpan () const {
		return TokenSpan::spanThrough(whileKeyword.getSpan(), closing.getSpan());
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(keyword.getSpan(), closing.getSpan());
	}
};

}
