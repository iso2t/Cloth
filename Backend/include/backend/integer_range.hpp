// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>

namespace backend::detail {

struct IntegerRange {
	LLVMValueRef start;
	LLVMValueRef end;
	LLVMValueRef inclusive; // Predicate in SSA, one byte in the aggregate.

	static LLVMTypeRef  type (LLVMTypeRef endpoint);
	static IntegerRange unpack (LLVMBuilderRef builder, LLVMValueRef value);
	static LLVMValueRef equalPacked (LLVMBuilderRef builder, LLVMValueRef left, LLVMValueRef right, bool isNullable);
	LLVMValueRef        pack (LLVMBuilderRef builder) const;
	LLVMValueRef        equal (LLVMBuilderRef builder, const IntegerRange &other) const;
};
}
