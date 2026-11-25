// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <stdatomic.h>
#include <stdint.h>

_Noreturn void cloth_rt_identity_exhausted (void);

uint64_t cloth_rt_new_identity (void) {
	// Serialize the 64-bit sequence even on targets without native 64-bit atomics.
	static atomic_flag lock = ATOMIC_FLAG_INIT;
	static uint64_t    last = 0;
	while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {
	}
	const uint64_t identity = last == UINT64_MAX ? 0 : ++last;
	atomic_flag_clear_explicit(&lock, memory_order_release);
	if (!identity) cloth_rt_identity_exhausted();
	return identity;
}
