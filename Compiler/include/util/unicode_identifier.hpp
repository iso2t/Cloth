// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cloth {

// Byte length of the first UTF-8 scalar, or zero if it is invalid in this position.
std::size_t identifierStartLength (std::string_view text);
std::size_t identifierContinueLength (std::string_view text);
std::string normalizeIdentifier (std::string_view spelling);

}
