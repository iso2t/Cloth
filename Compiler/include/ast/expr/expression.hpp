// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>

#include <token/token.hpp>

namespace cloth {

struct Expression {
	virtual ~Expression ()              = default;
	[[nodiscard]] virtual TokenSpan get_span () const = 0;
};

using ExpressionPtr = std::shared_ptr<const Expression>;

}
