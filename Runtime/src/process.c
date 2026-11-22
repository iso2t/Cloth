// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdint.h>
#include <stdlib.h>

_Noreturn void cloth_rt_exit (int32_t status) {
	exit(status);
}
