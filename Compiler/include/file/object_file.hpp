// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>

namespace cloth {
struct CheckedProgram;

// Replaces the output file after the executable's object has been emitted and written successfully.
void emitObjectFile (const CheckedProgram &program, const std::filesystem::path &output);
}
