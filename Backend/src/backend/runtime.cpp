// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/runtime.hpp>

#include <span>
#include <string_view>

namespace backend::detail {
namespace {
LLVMValueRef declareVoidFunction (LLVMModuleRef module, const char *name, std::span<LLVMTypeRef> parameters, bool noReturn = false) {
	if (const LLVMValueRef fn_existing = LLVMGetNamedFunction(module, name)) return fn_existing;
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_function =
	        LLVMFunctionType(LLVMVoidTypeInContext(ctx_context), parameters.data(), static_cast<unsigned>(parameters.size()), false);
	const LLVMValueRef fn_runtime = LLVMAddFunction(module, name, ty_function);
	LLVMSetFunctionCallConv(fn_runtime, LLVMCCallConv);
	if (noReturn) {
		constexpr std::string_view sv_attribute = "noreturn";
		const unsigned             u_kind       = LLVMGetEnumAttributeKindForName(sv_attribute.data(), sv_attribute.size());
		LLVMAddAttributeAtIndex(fn_runtime, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ctx_context, u_kind, 0));
	}
	return fn_runtime;
}
}

LLVMValueRef declarePrintlnInt32 (LLVMModuleRef module) {
	LLVMTypeRef v_parameters[] = { LLVMInt32TypeInContext(LLVMGetModuleContext(module)) };
	return declareVoidFunction(module, "cloth_rt_println_i32", v_parameters);
}

LLVMValueRef declareExit (LLVMModuleRef module) {
	LLVMTypeRef v_parameters[] = { LLVMInt32TypeInContext(LLVMGetModuleContext(module)) };
	return declareVoidFunction(module, "cloth_rt_exit", v_parameters, /*noReturn=*/true);
}

LLVMValueRef declareDivideByZero (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_divide_by_zero", {}, /*noReturn=*/true);
}

LLVMValueRef declareSignedDivisionOverflow (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_signed_division_overflow", {}, /*noReturn=*/true);
}

LLVMValueRef declareNegativeIntegerExponent (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_negative_integer_exponent", {}, /*noReturn=*/true);
}

LLVMValueRef declareInvalidFloatToInteger (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_invalid_float_to_integer", {}, /*noReturn=*/true);
}

LLVMValueRef declareArrayIndexOutOfBounds (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_array_index_out_of_bounds", {}, /*noReturn=*/true);
}
}
