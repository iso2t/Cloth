// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>

#include <token/token.hpp>

namespace cloth {

struct Statement {
	virtual ~Statement ()                             = default;
	[[nodiscard]] virtual TokenSpan get_span () const = 0;
};

using StatementPtr = std::shared_ptr<const Statement>;

}
