// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/stmt/loop_control.hpp>
#include <token/token.hpp>

namespace cloth {

struct WhileStatement : LoopStatement {
	Token         keyword;
	Token         opening;
	ExpressionPtr condition;
	Token         closing;
	LoopBody      body;
	LoopTargetPtr loopTarget = std::make_shared<LoopTarget>();

	WhileStatement (const Token &keyword, const Token &opening, ExpressionPtr condition, const Token &closing, LoopBody body) :
	        keyword(keyword),
	        opening(opening),
	        condition(std::move(condition)),
	        closing(closing),
	        body(std::move(body)) {
	}

	[[nodiscard]] const LoopTargetPtr &getLoopTarget () const override {
		return loopTarget;
	}

	[[nodiscard]] TokenSpan getHeaderSpan () const {
		return TokenSpan::spanThrough(keyword.getSpan(), closing.getSpan());
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(keyword.getSpan(), body.span);
	}
};

}
