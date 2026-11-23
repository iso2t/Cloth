// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <inttypes.h>
#include <stdio.h>

void cloth_rt_println_i32 (int32_t value) {
	printf("%" PRId32 "\n", value);
}
