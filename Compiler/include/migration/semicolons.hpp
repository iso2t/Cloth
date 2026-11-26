// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>

namespace cloth {
class SourceFile;

namespace migration {
enum class InputDialect { Unknown, Legacy };

// Return verified replacement bytes, or no value when the original can be kept.
std::optional<std::string> migrateSemicolons (const SourceFile &source, InputDialect dialect);
}
}
