// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>

namespace backend::detail {
[[nodiscard]] LLVMValueRef declarePrintlnInt32 (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareDivideByZero (LLVMModuleRef module);
[[nodiscard]] LLVMValueRef declareInvalidFloatToInteger (LLVMModuleRef module);
}
