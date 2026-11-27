// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <llvm-c/Core.h>

namespace backend::detail {

struct RangeValue {
	LLVMValueRef start;
	LLVMValueRef end;
	LLVMValueRef inclusive; // Predicate in SSA, one byte in the aggregate.

	static LLVMTypeRef  type (LLVMTypeRef endpoint);
	static RangeValue   unpack (LLVMBuilderRef builder, LLVMValueRef value);
	static LLVMValueRef equalPackedIntegers (LLVMBuilderRef builder, LLVMValueRef left, LLVMValueRef right, bool isNullable);
	LLVMValueRef        pack (LLVMBuilderRef builder) const;
	LLVMValueRef        equalIntegers (LLVMBuilderRef builder, const RangeValue &other) const;
};
}
