// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <vector>

#include <ast/stmt/statement.hpp>
#include <token/token.hpp>

namespace cloth {

/** Loop targets are compared by object identity. */
struct LoopTarget {};

using LoopTargetPtr = std::shared_ptr<const LoopTarget>;

struct LoopBody {
	std::vector<StatementPtr> statements;
	bool                      braced;
	TokenSpan                 span;
};

struct LoopStatement : Statement {
	[[nodiscard]] virtual const LoopTargetPtr &get_loop_target () const = 0;
};

struct LoopJumpStatement : Statement {
	Token keyword;

	explicit LoopJumpStatement (const Token &keyword) : keyword(keyword) {
	}

	[[nodiscard]] bool isContinue () const {
		return keyword.get_lexeme() == to_string(kContinue);
	}

	[[nodiscard]] TokenSpan get_span () const override {
		return keyword.get_span();
	}
};

}
