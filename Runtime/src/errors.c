// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>
#include <stdlib.h>

static _Noreturn void report_error (const char *message) {
	fflush(stdout);
	fprintf(stderr, "Cloth runtime error: %s\n", message);
	exit(1);
}

_Noreturn void cloth_rt_divide_by_zero (void) {
	report_error("integer division by zero");
}

_Noreturn void cloth_rt_invalid_float_to_integer (void) {
	report_error("invalid float-to-integer cast (NaN or out of range)");
}
