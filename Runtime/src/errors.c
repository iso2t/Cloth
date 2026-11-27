// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void clothRtError (const char *message) {
	clothRtErrorBytes((const unsigned char *)message, strlen(message));
}

_Noreturn void clothRtErrorBytes (const unsigned char *message, size_t length) {
	fflush(stdout);
	fputs("Cloth runtime error: ", stderr);
	if (length) fwrite(message, 1, length, stderr);
	fputc('\n', stderr);
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

_Noreturn void clothRtNonFiniteRangeEndpoint (void) {
	clothRtError("floating range endpoints must be finite");
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
