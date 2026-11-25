// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

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
LLVMValueRef literal (LLVMModuleRef module, const cloth::StringLiteral &value);
Bytes        bytes (LLVMBuilderRef builder, LLVMValueRef value);
LLVMValueRef scalarCount (LLVMBuilderRef builder, LLVMValueRef value);
}
