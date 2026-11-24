// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>

namespace backend::detail::nullable {

LLVMTypeRef  type (LLVMTypeRef payloadType);
LLVMValueRef presentConstant (LLVMValueRef payload);
LLVMValueRef pack (LLVMBuilderRef builder, LLVMValueRef payload, LLVMValueRef presence = nullptr);
LLVMValueRef presence (LLVMBuilderRef builder, LLVMValueRef value);
LLVMValueRef loadPresence (LLVMBuilderRef builder, LLVMValueRef address);
LLVMValueRef payload (LLVMBuilderRef builder, LLVMValueRef value);
LLVMValueRef payloadAddress (LLVMBuilderRef builder, LLVMTypeRef payloadType, LLVMValueRef address);
LLVMValueRef equal (LLVMBuilderRef builder, LLVMValueRef left, LLVMValueRef right, LLVMValueRef payloadsEqual);
}
