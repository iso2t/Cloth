// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <limits>

namespace cloth {
struct StorageLayout {
	std::uint64_t size; // Allocation size in bytes, including trailing padding.
	unsigned      alignment;
	bool          operator==(const StorageLayout &) const = default;
};

struct TargetLayout {
	unsigned sizeBits = std::numeric_limits<std::uintptr_t>::digits;
	// The 32-bit x86 System V ABI gives eight-byte scalars four-byte alignment.
#if (defined(__i386__) || defined(__x86_64__)) && !defined(_WIN32)
	unsigned scalar64Alignment32 = 4;
#else
	unsigned scalar64Alignment32 = 8;
#endif

	[[nodiscard]] StorageLayout integerStorage (unsigned width) const;

	[[nodiscard]] std::uint64_t maximumSize () const {
		return sizeBits == 32 ? UINT32_MAX : UINT64_MAX;
	}
};
}
