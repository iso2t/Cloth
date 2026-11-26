// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../include/cloth/abi.h"

struct ClothString;

CLOTH_RT_API(_Noreturn void, clothRtError, error, const char *message);
CLOTH_RT_API(_Noreturn void, clothRtDivideByZero, divide_by_zero, void);
CLOTH_RT_API(_Noreturn void, clothRtSignedDivisionOverflow, signed_division_overflow, void);
CLOTH_RT_API(_Noreturn void, clothRtNegativeIntegerExponent, negative_integer_exponent, void);
CLOTH_RT_API(_Noreturn void, clothRtInvalidFloatToInteger, invalid_float_to_integer, void);
CLOTH_RT_API(_Noreturn void, clothRtAmbiguousEquality, ambiguous_equality, void);
CLOTH_RT_API(_Noreturn void, clothRtArrayIndexOutOfBounds, array_index_out_of_bounds, void);
CLOTH_RT_API(_Noreturn void, clothRtArrayLengthMismatch, array_length_mismatch, void);
CLOTH_RT_API(_Noreturn void, clothRtArrayCapacityOverflow, array_capacity_overflow, void);
CLOTH_RT_API(_Noreturn void, clothRtIdentityExhausted, identity_exhausted, void);
CLOTH_RT_API(_Noreturn void, clothRtArrayAllocationFailed, array_allocation_failed, void);
CLOTH_RT_API(_Noreturn void, clothRtStalledRange, stalled_range, void);

CLOTH_RT_API(size_t, clothRtIntegerZeros, integer_zeros, const unsigned char *bits, uint32_t width, uint8_t leading);
CLOTH_RT_API(void, clothRtMulWords, mul_words, uint32_t *product, const uint32_t *left, const uint32_t *right, uint32_t wordCount);
CLOTH_RT_API(void, clothRtDivmodWords, divmod_words, uint32_t *dividend, uint32_t *divisor, uint32_t *remainder, const uint32_t wordCount,
             const uint32_t signedOperands);

CLOTH_RT_API(struct ClothString *, clothRtProcessArguments, process_arguments, int argc, char **argv, size_t *count);
CLOTH_RT_API(void, clothRtReleaseArguments, release_arguments, struct ClothString *arguments, size_t count);

CLOTH_RT_API(void, clothRtPrintlnI32, println_i32, int32_t value);

CLOTH_RT_API(_Noreturn void, clothRtExit, exit, int32_t status);
CLOTH_RT_API(_Noreturn void, clothRtExitMessage, exit_message, int32_t status, const unsigned char *data, size_t length);
