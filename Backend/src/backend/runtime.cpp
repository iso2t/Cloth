// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <backend/runtime.hpp>

#include <span>
#include <string_view>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>

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

LLVMValueRef declareIndexedArrayMutation (LLVMModuleRef module, const char *name) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	const LLVMTypeRef    ty_word        = LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module));
	LLVMTypeRef          v_parameters[] = { ty_pointer, ty_word, ty_pointer, ty_word, ty_word };
	return declareVoidFunction(module, name, v_parameters);
}
}

LLVMValueRef declareNewIdentity (LLVMModuleRef module) {
	constexpr char NAME[] = "cloth_rt_new_identity";
	if (const LLVMValueRef ir_existing = LLVMGetNamedFunction(module, NAME)) return ir_existing;
	const LLVMTypeRef ty_handle = LLVMInt64TypeInContext(LLVMGetModuleContext(module));
	return LLVMAddFunction(module, NAME, LLVMFunctionType(ty_handle, nullptr, 0, false));
}

LLVMValueRef declareProcessArguments (LLVMModuleRef module) {
	constexpr char NAME[] = "cloth_rt_process_arguments";
	if (const LLVMValueRef ir_existing = LLVMGetNamedFunction(module, NAME)) return ir_existing;
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	LLVMTypeRef          v_parameters[] = { LLVMInt32TypeInContext(ctx_context), ty_pointer, ty_pointer };
	return LLVMAddFunction(module, NAME, LLVMFunctionType(ty_pointer, v_parameters, 3, false));
}

LLVMValueRef declareReleaseArguments (LLVMModuleRef module) {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	LLVMTypeRef v_parameters[]       = { LLVMPointerTypeInContext(ctx_context, 0), LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module)) };
	return declareVoidFunction(module, "cloth_rt_release_arguments", v_parameters);
}

LLVMValueRef declareExit (LLVMModuleRef module) {
	LLVMTypeRef v_parameters[] = { LLVMInt32TypeInContext(LLVMGetModuleContext(module)) };
	return declareVoidFunction(module, "cloth_rt_exit", v_parameters, /*noReturn=*/true);
}

LLVMValueRef declareExitMessage (LLVMModuleRef module) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	LLVMTypeRef          v_parameters[] = { LLVMInt32TypeInContext(ctx_context), LLVMPointerTypeInContext(ctx_context, 0),
		                                LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module)) };
	return declareVoidFunction(module, "cloth_rt_exit_message", v_parameters, /*noReturn=*/true);
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

LLVMValueRef declareAmbiguousEquality (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_ambiguous_equality", {}, /*noReturn=*/true);
}

LLVMValueRef declareArrayIndexOutOfBounds (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_array_index_out_of_bounds", {}, /*noReturn=*/true);
}

LLVMValueRef declareArrayLengthMismatch (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_array_length_mismatch", {}, /*noReturn=*/true);
}

LLVMValueRef declareArrayAssign (LLVMModuleRef module) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	const LLVMTypeRef    ty_word        = LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module));
	LLVMTypeRef          v_parameters[] = { ty_pointer, ty_pointer, ty_word, ty_word, ty_word };
	return declareVoidFunction(module, "cloth_rt_array_assign", v_parameters);
}

LLVMValueRef declareArrayReserve (LLVMModuleRef module) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_word        = LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module));
	LLVMTypeRef          v_parameters[] = { LLVMPointerTypeInContext(ctx_context, 0), ty_word, ty_word, ty_word };
	return declareVoidFunction(module, "cloth_rt_array_reserve", v_parameters);
}

LLVMValueRef declareArrayAppend (LLVMModuleRef module) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	const LLVMTypeRef    ty_word        = LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module));
	LLVMTypeRef          v_parameters[] = { ty_pointer, ty_pointer, ty_word, ty_word };
	return declareVoidFunction(module, "cloth_rt_array_append", v_parameters);
}

LLVMValueRef declareArrayRelease (LLVMModuleRef module) {
	LLVMTypeRef v_parameters[] = { LLVMPointerTypeInContext(LLVMGetModuleContext(module), 0) };
	return declareVoidFunction(module, "cloth_rt_array_release", v_parameters);
}

LLVMValueRef declareArrayInsert (LLVMModuleRef module) {
	return declareIndexedArrayMutation(module, "cloth_rt_array_insert");
}

LLVMValueRef declareArrayRemoveAt (LLVMModuleRef module) {
	return declareIndexedArrayMutation(module, "cloth_rt_array_remove_at");
}

LLVMValueRef declareArrayClear (LLVMModuleRef module) {
	LLVMTypeRef v_parameters[] = { LLVMPointerTypeInContext(LLVMGetModuleContext(module), 0) };
	return declareVoidFunction(module, "cloth_rt_array_clear", v_parameters);
}

LLVMValueRef declareStalledRange (LLVMModuleRef module) {
	return declareVoidFunction(module, "cloth_rt_stalled_range", {}, /*noReturn=*/true);
}

LLVMValueRef declareMultiplyWords (LLVMModuleRef module) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	LLVMTypeRef          v_parameters[] = { ty_pointer, ty_pointer, ty_pointer, LLVMInt32TypeInContext(ctx_context) };
	return declareVoidFunction(module, "cloth_rt_mul_words", v_parameters);
}

LLVMValueRef declareDivideWords (LLVMModuleRef module) {
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	const LLVMTypeRef    ty_word        = LLVMInt32TypeInContext(ctx_context);
	LLVMTypeRef          v_parameters[] = { ty_pointer, ty_pointer, ty_pointer, ty_word, ty_word };
	return declareVoidFunction(module, "cloth_rt_divmod_words", v_parameters);
}
}
