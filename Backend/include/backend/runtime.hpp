// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Types.h>

namespace backend::detail {
[[nodiscard]] LLVMValueRef declareProcessArguments (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareReleaseArguments (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareNewIdentity (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareExit (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareExitMessage (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareDivideByZero (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareSignedDivisionOverflow (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareNegativeIntegerExponent (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareInvalidFloatToInteger (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareAmbiguousEquality (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayIndexOutOfBounds (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayLengthMismatch (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayAssign (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayReserve (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayAppend (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayInsert (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayRemoveAt (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayClear (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareArrayRelease (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareStalledRange (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareMultiplyWords (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareDivideWords (LLVMModuleRef module);
}
