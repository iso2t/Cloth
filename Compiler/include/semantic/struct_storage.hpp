// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <optional>

#include <semantic/target_layout.hpp>

namespace cloth {

// Fields start at their ABI alignment. The completed aggregate includes tail padding.
class StructStorage {
    public:
	explicit StructStorage (const TargetLayout &target);
	std::optional<std::uint64_t>               append (StorageLayout field);
	[[nodiscard]] std::optional<StorageLayout> finish (bool reserveEmptyByte = true) const;

    private:
	std::uint64_t u64_limit;
	std::uint64_t u64_end     = 0;
	unsigned      u_alignment = 1;
};
}
