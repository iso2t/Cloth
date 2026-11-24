// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <backend/backend.hpp>

#include <filesystem>

namespace cloth {
struct CheckedProgram;

// Replaces the destination after the executable's native output has been emitted and written successfully.
void emitFile (const CheckedProgram &program, backend::EmitKind kind, const std::filesystem::path &output);
}
