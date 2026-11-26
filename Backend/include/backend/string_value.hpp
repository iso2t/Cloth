// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include <llvm-c/Types.h>

namespace cloth {
struct StringLiteral;
}

namespace backend::detail::string_value {
struct Bytes {
	LLVMValueRef data;
	LLVMValueRef length;
};

LLVMTypeRef  type (LLVMModuleRef module);
// A contiguous, null-terminated LLVM constant, including for inputs larger than LLVM 18's string length limit.
LLVMValueRef literalBytes (LLVMContextRef context, std::string_view bytes);
LLVMValueRef constant (LLVMModuleRef module, std::string_view text);
LLVMValueRef literal (LLVMModuleRef module, const cloth::StringLiteral &value);
Bytes        bytes (LLVMBuilderRef builder, LLVMValueRef value);
LLVMValueRef scalarCount (LLVMBuilderRef builder, LLVMValueRef value);
}
