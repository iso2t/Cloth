// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/nullable_value.hpp>

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
}
