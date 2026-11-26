// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "runtime.h"

void clothRtPrintlnI32 (int32_t value) {
	printf("%" PRId32 "\n", value);
}
