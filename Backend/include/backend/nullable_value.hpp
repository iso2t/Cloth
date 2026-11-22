// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>
#include <semantic/checked_program.hpp>

namespace backend::detail::nullable {

bool         supportsPayload (const cloth::ResolvedType &payload);
LLVMTypeRef  type (LLVMTypeRef payloadType);
LLVMValueRef presentConstant (LLVMValueRef payload);
LLVMValueRef pack (LLVMBuilderRef builder, LLVMValueRef payload, LLVMValueRef presence = nullptr);
LLVMValueRef presence (LLVMBuilderRef builder, LLVMValueRef value);
LLVMValueRef payload (LLVMBuilderRef builder, LLVMValueRef value);
}
