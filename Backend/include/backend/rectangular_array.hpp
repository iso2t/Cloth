// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail::rectangular_array {

bool                supported (const cloth::ResolvedType &type);
cloth::ResolvedType leafType (cloth::ResolvedType type);
// Every row in a runtime-sized rectangular buffer is stored as a descriptor, even for a declared fixed extent.
cloth::ResolvedType openType (cloth::ResolvedType type);
LLVMValueRef        shape (LLVMTypeRef descriptor, cloth::ResolvedType array);

struct Layout {
	LLVMValueRef rank;
	LLVMValueRef stride;
	LLVMValueRef alignment;
};

Layout layout (LLVMModuleRef module, const cloth::CheckedProgram &program, const cloth::ResolvedType &type);
void   call (LLVMBuilderRef builder, LLVMModuleRef module, const char *name, std::initializer_list<LLVMValueRef> arguments);
}
