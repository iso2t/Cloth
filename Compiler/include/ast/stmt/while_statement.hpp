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

	const LoopTargetPtr &get_loop_target () const override {
		return loopTarget;
	}

	TokenSpan get_header_span () const {
		return TokenSpan::span_through(keyword.get_span(), closing.get_span());
	}

	TokenSpan get_span () const override {
		return TokenSpan::span_through(keyword.get_span(), body.span);
	}
};

}
