// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cloth {
struct CheckedProgram;
}

namespace backend {

enum class EmitKind { Object, Assembly };

[[nodiscard]] std::string toolchainTriple (unsigned sizeBits);

// Returns native output bytes for a checked executable or named library. An explicit LLVM triple must match its layout.
// An empty triple selects the Cloth toolchain at the checked pointer width.
[[nodiscard]] std::vector<std::byte> emit (const cloth::CheckedProgram &program, EmitKind kind, std::string_view targetTriple = {});
[[nodiscard]] std::vector<std::byte> emitObject (const cloth::CheckedProgram &program, std::string_view targetTriple = {});

}
