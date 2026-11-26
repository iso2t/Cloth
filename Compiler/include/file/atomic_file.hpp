// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace cloth {

// Publish a fully written sibling in one rename.
void publishFile (const std::filesystem::path &output, std::span<const std::byte> bytes);
}
