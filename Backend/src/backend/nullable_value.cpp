// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/nullable_value.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

namespace backend::detail::nullable {
namespace {
constexpr unsigned PRESENCE = 0, PAYLOAD = 1;
}

LLVMTypeRef type (LLVMTypeRef payloadType) {
	const LLVMContextRef ctx_context = LLVMGetTypeContext(payloadType);
	LLVMTypeRef          v_fields[]  = { LLVMInt8TypeInContext(ctx_context), payloadType };
	return LLVMStructTypeInContext(ctx_context, v_fields, 2, false);
}

LLVMValueRef presentConstant (LLVMValueRef payload) {
	const LLVMContextRef ctx_context = LLVMGetTypeContext(LLVMTypeOf(payload));
	LLVMValueRef         v_fields[]  = { LLVMConstInt(LLVMInt8TypeInContext(ctx_context), 1, false), payload };
	return LLVMConstStructInContext(ctx_context, v_fields, 2, false);
}

LLVMValueRef pack (LLVMBuilderRef builder, LLVMValueRef payload, LLVMValueRef presence) {
	LLVMValueRef ir_value = presentConstant(LLVMConstNull(LLVMTypeOf(payload)));
	if (presence) ir_value = LLVMBuildInsertValue(builder, ir_value, presence, PRESENCE, "");
	return LLVMBuildInsertValue(builder, ir_value, payload, PAYLOAD, "");
}

LLVMValueRef presence (LLVMBuilderRef builder, LLVMValueRef value) {
	return LLVMBuildExtractValue(builder, value, PRESENCE, "");
}

LLVMValueRef payload (LLVMBuilderRef builder, LLVMValueRef value) {
	return LLVMBuildExtractValue(builder, value, PAYLOAD, "");
}

LLVMValueRef payloadAddress (LLVMBuilderRef builder, LLVMTypeRef payloadType, LLVMValueRef address) {
	return LLVMBuildStructGEP2(builder, type(payloadType), address, PAYLOAD, "");
}

LLVMValueRef equal (LLVMBuilderRef builder, LLVMValueRef left, LLVMValueRef right, LLVMValueRef payloadsEqual) {
	const LLVMValueRef ir_leftPresent  = LLVMBuildIsNotNull(builder, presence(builder, left), "");
	const LLVMValueRef ir_rightPresent = LLVMBuildIsNotNull(builder, presence(builder, right), "");
	const LLVMValueRef ir_bothPresent  = LLVMBuildAnd(builder, ir_leftPresent, ir_rightPresent, "");
	const LLVMValueRef ir_samePresence = LLVMBuildICmp(builder, LLVMIntEQ, ir_leftPresent, ir_rightPresent, "");
	// An absent payload can be poison. Only select the payload comparison when both values are present.
	return LLVMBuildSelect(builder, ir_bothPresent, payloadsEqual, ir_samePresence, "");
}
}
