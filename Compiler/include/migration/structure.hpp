// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include <token/token.hpp>

namespace cloth {
struct SourceUnit;

namespace migration {
// Return the first differing part of the reference parse in original source coordinates.
std::optional<TokenSpan> firstParseDifference (const SourceUnit &reference, const SourceUnit &candidate);
}
}
