// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

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

_Noreturn void cloth_rt_signed_division_overflow (void) {
	report_error("signed integer division overflow");
}

_Noreturn void cloth_rt_negative_integer_exponent (void) {
	report_error("negative integer exponent");
}

_Noreturn void cloth_rt_invalid_float_to_integer (void) {
	report_error("invalid float-to-integer cast (NaN or out of range)");
}

_Noreturn void cloth_rt_ambiguous_equality (void) {
	report_error("ambiguous reference equality overload");
}

_Noreturn void cloth_rt_array_index_out_of_bounds (void) {
	report_error("array index out of bounds");
}

_Noreturn void cloth_rt_array_length_mismatch (void) {
	report_error("array length does not match fixed extent");
}

_Noreturn void cloth_rt_array_capacity_overflow (void) {
	report_error("array capacity exceeds addressable storage");
}

_Noreturn void cloth_rt_identity_exhausted (void) {
	report_error("object identity space exhausted");
}

_Noreturn void cloth_rt_array_allocation_failed (void) {
	report_error("unable to allocate array storage");
}

_Noreturn void cloth_rt_stalled_range (void) {
	report_error("range counter update made no progress");
}
