// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <runtime.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void clothRtError (const char *message) {
	fflush(stdout);
	fprintf(stderr, "Cloth runtime error: %s\n", message);
	exit(1);
}

_Noreturn void clothRtDivideByZero (void) {
	clothRtError("integer division by zero");
}

_Noreturn void clothRtSignedDivisionOverflow (void) {
	clothRtError("signed integer division overflow");
}

_Noreturn void clothRtNegativeIntegerExponent (void) {
	clothRtError("negative integer exponent");
}

_Noreturn void clothRtInvalidFloatToInteger (void) {
	clothRtError("invalid float-to-integer cast (NaN or out of range)");
}

_Noreturn void clothRtAmbiguousEquality (void) {
	clothRtError("ambiguous reference equality overload");
}

_Noreturn void clothRtArrayIndexOutOfBounds (void) {
	clothRtError("array index out of bounds");
}

_Noreturn void clothRtArrayLengthMismatch (void) {
	clothRtError("array length does not match fixed extent");
}

_Noreturn void clothRtArrayCapacityOverflow (void) {
	clothRtError("array capacity exceeds addressable storage");
}

_Noreturn void clothRtIdentityExhausted (void) {
	clothRtError("object identity space exhausted");
}

_Noreturn void clothRtArrayAllocationFailed (void) {
	clothRtError("unable to allocate array storage");
}

_Noreturn void clothRtStalledRange (void) {
	clothRtError("range counter update made no progress");
}
