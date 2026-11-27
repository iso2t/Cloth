// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/nullable_value.hpp>
#include <backend/range_value.hpp>

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

namespace backend::detail {
namespace {
enum Field : unsigned { Start, End, Inclusive, FieldCount };
}

LLVMTypeRef RangeValue::type (LLVMTypeRef endpoint) {
	const LLVMContextRef ctx_context = LLVMGetTypeContext(endpoint);
	LLVMTypeRef          v_fields[]  = { endpoint, endpoint, LLVMInt8TypeInContext(ctx_context) };
	return LLVMStructTypeInContext(ctx_context, v_fields, FieldCount, false);
}

RangeValue RangeValue::unpack (LLVMBuilderRef builder, LLVMValueRef value) {
	return { .start     = LLVMBuildExtractValue(builder, value, Start, ""),
		 .end       = LLVMBuildExtractValue(builder, value, End, ""),
		 .inclusive = LLVMBuildIsNotNull(builder, LLVMBuildExtractValue(builder, value, Inclusive, ""), "") };
}

LLVMValueRef RangeValue::pack (LLVMBuilderRef builder) const {
	const LLVMTypeRef ty_range = type(LLVMTypeOf(start));
	LLVMValueRef      ir_value = LLVMBuildInsertValue(builder, LLVMGetUndef(ty_range), start, Start, "");
	ir_value                   = LLVMBuildInsertValue(builder, ir_value, end, End, "");
	const LLVMValueRef ir_flag = LLVMBuildZExt(builder, inclusive, LLVMStructGetTypeAtIndex(ty_range, Inclusive), "");
	return LLVMBuildInsertValue(builder, ir_value, ir_flag, Inclusive, "");
}

LLVMValueRef RangeValue::equalIntegers (LLVMBuilderRef builder, const RangeValue &other) const {
	const LLVMValueRef ir_sameStart     = LLVMBuildICmp(builder, LLVMIntEQ, start, other.start, "");
	const LLVMValueRef ir_sameEnd       = LLVMBuildICmp(builder, LLVMIntEQ, end, other.end, "");
	const LLVMValueRef ir_sameInclusion = LLVMBuildICmp(builder, LLVMIntEQ, inclusive, other.inclusive, "");
	return LLVMBuildAnd(builder, LLVMBuildAnd(builder, ir_sameStart, ir_sameEnd, ""), ir_sameInclusion, "");
}

LLVMValueRef RangeValue::equalPackedIntegers (LLVMBuilderRef builder, LLVMValueRef left, LLVMValueRef right, bool isNullable) {
	const RangeValue   range_left  = unpack(builder, isNullable ? nullable::payload(builder, left) : left);
	const RangeValue   range_right = unpack(builder, isNullable ? nullable::payload(builder, right) : right);
	const LLVMValueRef ir_equal    = range_left.equalIntegers(builder, range_right);
	return isNullable ? nullable::equal(builder, left, right, ir_equal) : ir_equal;
}
}
