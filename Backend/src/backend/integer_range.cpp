// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/integer_range.hpp>
#include <backend/nullable_value.hpp>

namespace backend::detail {
namespace {
enum Field : unsigned { Start, End, Inclusive, FieldCount };
}

LLVMTypeRef IntegerRange::type (LLVMTypeRef endpoint) {
	const LLVMContextRef ctx_context = LLVMGetTypeContext(endpoint);
	LLVMTypeRef          v_fields[]  = { endpoint, endpoint, LLVMInt8TypeInContext(ctx_context) };
	return LLVMStructTypeInContext(ctx_context, v_fields, FieldCount, false);
}

IntegerRange IntegerRange::unpack (LLVMBuilderRef builder, LLVMValueRef value) {
	return { LLVMBuildExtractValue(builder, value, Start, ""), LLVMBuildExtractValue(builder, value, End, ""),
		 LLVMBuildIsNotNull(builder, LLVMBuildExtractValue(builder, value, Inclusive, ""), "") };
}

LLVMValueRef IntegerRange::pack (LLVMBuilderRef builder) const {
	const LLVMTypeRef ty_range = type(LLVMTypeOf(start));
	LLVMValueRef      ir_value = LLVMBuildInsertValue(builder, LLVMGetUndef(ty_range), start, Start, "");
	ir_value                   = LLVMBuildInsertValue(builder, ir_value, end, End, "");
	const LLVMValueRef ir_flag = LLVMBuildZExt(builder, inclusive, LLVMStructGetTypeAtIndex(ty_range, Inclusive), "");
	return LLVMBuildInsertValue(builder, ir_value, ir_flag, Inclusive, "");
}

LLVMValueRef IntegerRange::equal (LLVMBuilderRef builder, const IntegerRange &other) const {
	const LLVMValueRef ir_sameStart     = LLVMBuildICmp(builder, LLVMIntEQ, start, other.start, "");
	const LLVMValueRef ir_sameEnd       = LLVMBuildICmp(builder, LLVMIntEQ, end, other.end, "");
	const LLVMValueRef ir_sameInclusion = LLVMBuildICmp(builder, LLVMIntEQ, inclusive, other.inclusive, "");
	return LLVMBuildAnd(builder, LLVMBuildAnd(builder, ir_sameStart, ir_sameEnd, ""), ir_sameInclusion, "");
}

LLVMValueRef IntegerRange::equalPacked (LLVMBuilderRef builder, LLVMValueRef left, LLVMValueRef right, bool isNullable) {
	const IntegerRange range_left  = unpack(builder, isNullable ? nullable::payload(builder, left) : left);
	const IntegerRange range_right = unpack(builder, isNullable ? nullable::payload(builder, right) : right);
	const LLVMValueRef ir_equal    = range_left.equal(builder, range_right);
	return isNullable ? nullable::equal(builder, left, right, ir_equal) : ir_equal;
}
}
