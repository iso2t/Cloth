// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>

namespace backend::detail {
[[nodiscard]] LLVMValueRef declarePrintlnInt32 (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareExit (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareDivideByZero (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareSignedDivisionOverflow (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareNegativeIntegerExponent (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareInvalidFloatToInteger (LLVMModuleRef module);
}
