// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <limits>

namespace cloth {
struct TargetLayout {
	unsigned sizeBits = std::numeric_limits<std::uintptr_t>::digits;
};
}
