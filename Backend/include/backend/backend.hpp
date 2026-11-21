// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace cloth {
struct CheckedProgram;
}

namespace backend {

// Returns the bytes of a relocatable object for a checked executable. An explicit LLVM triple must match its layout.
// An empty triple selects the Cloth toolchain at the checked pointer width.
[[nodiscard]] std::vector<std::byte> emitObject (const cloth::CheckedProgram &program, std::string_view targetTriple = {});

}
