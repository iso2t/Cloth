// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Types.h>
#include <semantic/target_layout.hpp>

namespace cloth {
struct ResolvedType;
}

namespace backend::detail::growable_array {

struct Elements {
	LLVMValueRef data;
	LLVMValueRef length;
};

[[nodiscard]] bool supported (const cloth::ResolvedType &type);
[[nodiscard]] bool ownsBuffer (const cloth::ResolvedType &type);
LLVMValueRef       borrow (LLVMBuilderRef builder, LLVMTypeRef descriptorType, LLVMValueRef data, LLVMValueRef length);
Elements           elements (LLVMBuilderRef builder, LLVMValueRef descriptor);
void assign (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef data, LLVMValueRef count, cloth::StorageLayout cell, LLVMValueRef operations = nullptr);
void reserve (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef minimum, cloth::StorageLayout cell);
void append (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef cellAddress, cloth::StorageLayout cell);
void insert (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef index, LLVMValueRef cellAddress, cloth::StorageLayout cell);
void removeAt (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef index, LLVMValueRef removed, cloth::StorageLayout cell);
void clear (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef operations = nullptr);
void release (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef operations = nullptr);
}
