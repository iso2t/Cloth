// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace cloth {
class SourceFile;

// Publish a fully written sibling in one rename. For a source rewrite, retain file permissions
// and recheck the original contents immediately before replacement.
void publishFile (const std::filesystem::path &output, std::span<const std::byte> bytes, const SourceFile *original = nullptr);
}
