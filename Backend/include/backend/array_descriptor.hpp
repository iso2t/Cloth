// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail::array_descriptor {

LLVMTypeRef type (LLVMModuleRef module, const cloth::CheckedProgram &program, const cloth::ResolvedType &array);
// A retained shape records dimensions without keeping a buffer or its capacity alive.
LLVMValueRef fixedWitness (LLVMTypeRef descriptorType, std::uint64_t count, LLVMValueRef rowWitness);
LLVMValueRef growableWitness (LLVMBuilderRef builder, LLVMValueRef nullableArray, LLVMValueRef previous);
// Descriptors for nullable-row trees share the fixed extents, one entry per dimension.
LLVMValueRef sharedShape (LLVMModuleRef module, LLVMValueRef fixedWitness);
LLVMValueRef childShape (LLVMModuleRef module, LLVMValueRef parentShape);
LLVMValueRef fixed (LLVMBuilderRef builder, LLVMValueRef fixedWitness, LLVMValueRef buffer, LLVMValueRef sharedShape);
LLVMValueRef dataAddress (LLVMBuilderRef builder, LLVMValueRef slot);
}
