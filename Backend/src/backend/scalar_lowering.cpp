// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/runtime.hpp>
#include <backend/scalar_lowering.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace backend::detail {
namespace {
LLVMValueRef integerConstant (LLVMTypeRef type, std::uint64_t value) {
	return LLVMConstInt(type, value, false);
}

LLVMIntPredicate integerPredicate (cloth::Operator op, bool isUnsigned) {
	switch (op) {
	case cloth::oEqual:
		return LLVMIntEQ;
	case cloth::oNotEqual:
		return LLVMIntNE;
	case cloth::oLess:
		return isUnsigned ? LLVMIntULT : LLVMIntSLT;
	case cloth::oLessEqual:
		return isUnsigned ? LLVMIntULE : LLVMIntSLE;
	case cloth::oGreater:
		return isUnsigned ? LLVMIntUGT : LLVMIntSGT;
	case cloth::oGreaterEqual:
		return isUnsigned ? LLVMIntUGE : LLVMIntSGE;
	default:
		throw std::logic_error("Invalid primitive integer comparison");
	}
}

LLVMRealPredicate floatPredicate (cloth::Operator op) {
	switch (op) {
	case cloth::oEqual:
		return LLVMRealOEQ;
	case cloth::oNotEqual:
		return LLVMRealUNE;
	case cloth::oLess:
		return LLVMRealOLT;
	case cloth::oLessEqual:
		return LLVMRealOLE;
	case cloth::oGreater:
		return LLVMRealOGT;
	case cloth::oGreaterEqual:
		return LLVMRealOGE;
	default:
		throw std::logic_error("Invalid primitive floating comparison");
	}
}

LLVMValueRef intrinsic (LLVMModuleRef module, std::string_view name, LLVMTypeRef *types = nullptr, unsigned count = 0) {
	return LLVMGetIntrinsicDeclaration(module, LLVMLookupIntrinsicID(name.data(), name.size()), types, count);
}

LLVMValueRef declareTrap (LLVMModuleRef module) {
	return intrinsic(module, "llvm.trap");
}
}

LLVMTypeRef scalarType (LLVMContextRef context, cloth::TargetLayout layout, const cloth::ResolvedType &type) {
	if (type.declaration || type.parameter || !type.suffixes.empty()) return nullptr;
	if (type.integer()) return LLVMIntTypeInContext(context, type.width ? type.width : layout.sizeBits);
	if (type == cloth::ResolvedType(cloth::kBoolean)) return LLVMInt8TypeInContext(context);
	if (type == cloth::ResolvedType(cloth::kCharacter)) return LLVMInt32TypeInContext(context);
	if (type.floating()) {
		switch (type.width) {
		case 16:
			return LLVMHalfTypeInContext(context);
		case 32:
			return LLVMFloatTypeInContext(context);
		case 64:
			return LLVMDoubleTypeInContext(context);
		}
	}
	return nullptr;
}

ScalarLowering::ScalarLowering (LLVMModuleRef module, LLVMBuilderRef builder, cloth::TargetLayout layout) :
        mod_module(module),
        ir_builder(builder),
        tl_layout(layout) {
}

LLVMTypeRef ScalarLowering::type (const cloth::ResolvedType &resolved) const {
	return scalarType(LLVMGetModuleContext(mod_module), tl_layout, resolved);
}

LLVMValueRef ScalarLowering::constant (const cloth::ResolvedType &resolved, const cloth::NumericConstant &value) const {
	const LLVMTypeRef ty_value = type(resolved);
	if (const double *d_real = std::get_if<double>(&value)) return LLVMConstReal(ty_value, *d_real);
	const cloth::IntegerConstant &ic_value = std::get<cloth::IntegerConstant>(value);
	// Cloth stores little-endian 32-bit magnitude words. LLVM consumes little-endian 64-bit limbs.
	std::vector<std::uint64_t> v_words((ic_value.magnitude.size() + 1) / 2);
	for (std::size_t st_index = 0; st_index < ic_value.magnitude.size(); ++st_index)
		v_words[st_index / 2] |= static_cast<std::uint64_t>(ic_value.magnitude[st_index]) << (32 * (st_index % 2));
	const LLVMValueRef ir_value = LLVMConstIntOfArbitraryPrecision(ty_value, static_cast<unsigned>(v_words.size()), v_words.data());
	return ic_value.negative ? LLVMConstNeg(ir_value) : ir_value;
}

LLVMValueRef ScalarLowering::predicate (LLVMValueRef integer) const {
	return LLVMBuildICmp(ir_builder, LLVMIntNE, integer, LLVMConstNull(LLVMTypeOf(integer)), "");
}

LLVMValueRef ScalarLowering::boolean (LLVMValueRef predicate) const {
	return LLVMBuildZExt(ir_builder, predicate, LLVMInt8TypeInContext(LLVMGetModuleContext(mod_module)), "");
}

// The x86 toolchains use different calling conventions for half-conversion libcalls.
// Encode/decode IEEE binary16 in the module so calls, locals and casts share one representation.
LLVMValueRef ScalarLowering::convertHalf (LLVMValueRef value) const {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	const bool           b_extend    = LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMHalfTypeKind;
	const LLVMTypeRef    ty_result   = b_extend ? LLVMFloatTypeInContext(ctx_context) : LLVMHalfTypeInContext(ctx_context);
	if (!b_extend) value = LLVMBuildFPCast(ir_builder, value, LLVMDoubleTypeInContext(ctx_context), "");
	if (LLVMIsAConstantFP(value)) return LLVMBuildFPCast(ir_builder, value, ty_result, "");

	const char  *s_name     = b_extend ? "cloth.fp16.extend" : "cloth.fp16.round";
	LLVMValueRef fn_convert = LLVMGetNamedFunction(mod_module, s_name);
	if (!fn_convert) {
		LLVMTypeRef ty_input = LLVMTypeOf(value);
		fn_convert           = LLVMAddFunction(mod_module, s_name, LLVMFunctionType(ty_result, &ty_input, 1, false));
		LLVMSetLinkage(fn_convert, LLVMInternalLinkage);
		const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_body(LLVMCreateBuilderInContext(ctx_context), LLVMDisposeBuilder);
		LLVMPositionBuilderAtEnd(ir_body.get(), LLVMAppendBasicBlockInContext(ctx_context, fn_convert, "entry"));
		const ScalarLowering sl_body(mod_module, ir_body.get(), tl_layout);
		const LLVMValueRef   ir_input = LLVMGetParam(fn_convert, 0);
		LLVMBuildRet(ir_body.get(), b_extend ? sl_body.extendHalf(ir_input) : sl_body.roundHalf(ir_input));
	}
	return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_convert), fn_convert, &value, 1, "");
}

LLVMValueRef ScalarLowering::extendHalf (LLVMValueRef value) const {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    ty_i32 = LLVMInt32TypeInContext(ctx_context), ty_float = LLVMFloatTypeInContext(ctx_context);
	const LLVMValueRef   ir_bits = LLVMBuildZExt(ir_builder, LLVMBuildBitCast(ir_builder, value, LLVMInt16TypeInContext(ctx_context), ""), ty_i32, "");
	const LLVMValueRef   ir_sign =
	        LLVMBuildShl(ir_builder, LLVMBuildAnd(ir_builder, ir_bits, integerConstant(ty_i32, 0x8000), ""), integerConstant(ty_i32, 16), "");
	const LLVMValueRef ir_abs      = LLVMBuildAnd(ir_builder, ir_bits, integerConstant(ty_i32, 0x7fff), "");
	const LLVMValueRef ir_fraction = LLVMBuildAnd(ir_builder, ir_abs, integerConstant(ty_i32, 0x3ff), "");
	const LLVMValueRef ir_normal =
	        LLVMBuildAdd(ir_builder, LLVMBuildShl(ir_builder, ir_abs, integerConstant(ty_i32, 13), ""), integerConstant(ty_i32, (127 - 15) << 23), "");
	const LLVMValueRef ir_special =
	        LLVMBuildOr(ir_builder, LLVMBuildShl(ir_builder, ir_fraction, integerConstant(ty_i32, 13), ""), integerConstant(ty_i32, 0x7f800000), "");
	// A subnormal is exactly its fraction times 2^-24, including zero.
	const LLVMValueRef ir_small = LLVMBuildFMul(ir_builder, LLVMBuildUIToFP(ir_builder, ir_fraction, ty_float, ""), LLVMConstReal(ty_float, 0x1p-24), "");
	LLVMValueRef       ir_result =
	        LLVMBuildSelect(ir_builder, LLVMBuildICmp(ir_builder, LLVMIntUGE, ir_abs, integerConstant(ty_i32, 0x7c00), ""), ir_special, ir_normal, "");
	ir_result = LLVMBuildSelect(ir_builder, LLVMBuildICmp(ir_builder, LLVMIntULT, ir_abs, integerConstant(ty_i32, 0x400), ""),
	                            LLVMBuildBitCast(ir_builder, ir_small, ty_i32, ""), ir_result, "");
	return LLVMBuildBitCast(ir_builder, LLVMBuildOr(ir_builder, ir_sign, ir_result, ""), ty_float, "");
}

LLVMValueRef ScalarLowering::roundHalf (LLVMValueRef value) const {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    ty_i64      = LLVMInt64TypeInContext(ctx_context);
	const LLVMValueRef   ir_bits = LLVMBuildBitCast(ir_builder, LLVMBuildFPCast(ir_builder, value, LLVMDoubleTypeInContext(ctx_context), ""), ty_i64, "");
	const LLVMValueRef   ir_sign =
	        LLVMBuildAnd(ir_builder, LLVMBuildLShr(ir_builder, ir_bits, integerConstant(ty_i64, 48), ""), integerConstant(ty_i64, 0x8000), "");
	const LLVMValueRef ir_exp =
	        LLVMBuildAnd(ir_builder, LLVMBuildLShr(ir_builder, ir_bits, integerConstant(ty_i64, 52), ""), integerConstant(ty_i64, 0x7ff), "");
	const LLVMValueRef ir_fraction    = LLVMBuildAnd(ir_builder, ir_bits, integerConstant(ty_i64, (UINT64_C(1) << 52) - 1), "");
	const LLVMValueRef ir_significand = LLVMBuildOr(ir_builder, ir_fraction, integerConstant(ty_i64, UINT64_C(1) << 52), "");
	// Normal results discard 42 fraction bits. Subnormals discard up to eleven more.
	constexpr unsigned U_MIN_NORMAL = 1023 - 14, U_HALF_SUBNORMAL = 1023 - 25;
	const LLVMValueRef ir_normal  = LLVMBuildICmp(ir_builder, LLVMIntUGE, ir_exp, integerConstant(ty_i64, U_MIN_NORMAL), "");
	const LLVMValueRef ir_tiny    = LLVMBuildICmp(ir_builder, LLVMIntULT, ir_exp, integerConstant(ty_i64, U_HALF_SUBNORMAL), "");
	LLVMValueRef       ir_clamped = LLVMBuildSelect(ir_builder, ir_tiny, integerConstant(ty_i64, U_HALF_SUBNORMAL), ir_exp, "");
	ir_clamped                    = LLVMBuildSelect(ir_builder, ir_normal, integerConstant(ty_i64, U_MIN_NORMAL), ir_clamped, "");
	const LLVMValueRef ir_shift   = LLVMBuildSub(ir_builder, integerConstant(ty_i64, 42 + U_MIN_NORMAL), ir_clamped, "");
	const LLVMValueRef ir_lastBit = LLVMBuildAnd(ir_builder, LLVMBuildLShr(ir_builder, ir_significand, ir_shift, ""), integerConstant(ty_i64, 1), "");
	const LLVMValueRef ir_halfway =
	        LLVMBuildShl(ir_builder, integerConstant(ty_i64, 1), LLVMBuildSub(ir_builder, ir_shift, integerConstant(ty_i64, 1), ""), "");
	const LLVMValueRef ir_bias    = LLVMBuildAdd(ir_builder, LLVMBuildSub(ir_builder, ir_halfway, integerConstant(ty_i64, 1), ""), ir_lastBit, "");
	const LLVMValueRef ir_rounded = LLVMBuildLShr(ir_builder, LLVMBuildAdd(ir_builder, ir_significand, ir_bias, ""), ir_shift, "");
	const LLVMValueRef ir_exponentBits =
	        LLVMBuildShl(ir_builder, LLVMBuildSub(ir_builder, ir_exp, integerConstant(ty_i64, U_MIN_NORMAL), ""), integerConstant(ty_i64, 10), "");
	LLVMValueRef ir_abs = LLVMBuildAdd(ir_builder, ir_rounded, LLVMBuildSelect(ir_builder, ir_normal, ir_exponentBits, integerConstant(ty_i64, 0), ""), "");
	ir_abs              = LLVMBuildSelect(ir_builder, ir_tiny, integerConstant(ty_i64, 0), ir_abs, "");
	ir_abs              = LLVMBuildSelect(ir_builder, LLVMBuildICmp(ir_builder, LLVMIntUGT, ir_exp, integerConstant(ty_i64, 1023 + 15), ""),
	                                      integerConstant(ty_i64, 0x7c00), ir_abs, "");
	const LLVMValueRef ir_nan =
	        LLVMBuildAnd(ir_builder, LLVMBuildICmp(ir_builder, LLVMIntEQ, ir_exp, integerConstant(ty_i64, 0x7ff), ""), predicate(ir_fraction), "");
	ir_abs                         = LLVMBuildSelect(ir_builder, ir_nan, integerConstant(ty_i64, 0x7e00), ir_abs, "");
	const LLVMValueRef ir_halfBits = LLVMBuildTrunc(ir_builder, LLVMBuildOr(ir_builder, ir_sign, ir_abs, ""), LLVMInt16TypeInContext(ctx_context), "");
	return LLVMBuildBitCast(ir_builder, ir_halfBits, LLVMHalfTypeInContext(ctx_context), "");
}

void ScalarLowering::failIf (LLVMValueRef condition, FailureDeclaration declareFailure) const {
	if (LLVMIsNull(condition)) return;
	const LLVMContextRef    ctx_context = LLVMGetModuleContext(mod_module);
	const LLVMValueRef      fn_function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ir_builder));
	const LLVMBasicBlockRef bb_failure  = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "failure");
	const LLVMBasicBlockRef bb_valid    = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "valid");
	LLVMBuildCondBr(ir_builder, condition, bb_failure, bb_valid);
	LLVMPositionBuilderAtEnd(ir_builder, bb_failure);
	const LLVMValueRef fn_failure = declareFailure(mod_module);
	LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_failure), fn_failure, nullptr, 0, "");
	LLVMBuildUnreachable(ir_builder);
	LLVMPositionBuilderAtEnd(ir_builder, bb_valid);
}

void ScalarLowering::guardDivisor (LLVMValueRef dividend, LLVMValueRef divisor, bool isUnsigned) const {
	const LLVMTypeRef ty_integer = LLVMTypeOf(divisor);
	failIf(LLVMBuildICmp(ir_builder, LLVMIntEQ, divisor, LLVMConstNull(ty_integer), ""), declareDivideByZero);
	if (isUnsigned) return;
	const LLVMValueRef ir_minusOne = LLVMBuildICmp(ir_builder, LLVMIntEQ, divisor, LLVMConstAllOnes(ty_integer), "");
	if (LLVMIsNull(ir_minusOne)) return;
	const LLVMValueRef ir_shift = LLVMConstInt(ty_integer, LLVMGetIntTypeWidth(ty_integer) - 1, false);
	const LLVMValueRef ir_min   = LLVMBuildShl(ir_builder, LLVMConstInt(ty_integer, 1, false), ir_shift, "");
	failIf(LLVMBuildAnd(ir_builder, ir_minusOne, LLVMBuildICmp(ir_builder, LLVMIntEQ, dividend, ir_min, ""), ""), declareTrap);
}

LLVMValueRef ScalarLowering::floatToInteger (LLVMValueRef value, LLVMTypeRef target, bool isUnsigned) const {
	// fpto[su]i truncates toward zero. Check the open interval around the representable integers before emitting it.
	const LLVMTypeRef  ty_double = LLVMDoubleTypeInContext(LLVMGetModuleContext(mod_module));
	const LLVMValueRef ir_wide   = LLVMBuildFPCast(ir_builder, value, ty_double, "");
	const double       d_upper   = std::ldexp(1., static_cast<int>(LLVMGetIntTypeWidth(target)) - !isUnsigned);
	const double       d_minimum = isUnsigned ? 0. : -d_upper;
	const double       d_lower   = d_minimum - 1.;
	// At large widths the extra unit rounds away. An infinite bound must still exclude infinities.
	const LLVMRealPredicate p_lower  = d_lower == d_minimum && std::isfinite(d_lower) ? LLVMRealOGE : LLVMRealOGT;
	const LLVMValueRef      ir_lower = LLVMBuildFCmp(ir_builder, p_lower, ir_wide, LLVMConstReal(ty_double, d_lower), "");
	const LLVMValueRef      ir_upper = LLVMBuildFCmp(ir_builder, LLVMRealOLT, ir_wide, LLVMConstReal(ty_double, d_upper), "");
	failIf(LLVMBuildNot(ir_builder, LLVMBuildAnd(ir_builder, ir_lower, ir_upper, ""), ""), declareInvalidFloatToInteger);
	return isUnsigned ? LLVMBuildFPToUI(ir_builder, value, target, "") : LLVMBuildFPToSI(ir_builder, value, target, "");
}

LLVMValueRef ScalarLowering::convert (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ResolvedType &destination) const {
	if (destination.convertsIntegerToBool(source)) return boolean(predicate(value));
	const LLVMTypeRef ty_target = type(destination);
	if (LLVMTypeOf(value) == ty_target) return value;
	if (source.floating()) {
		if (source.width == 16) value = convertHalf(value);
		if (destination.floating()) return destination.width == 16 ? convertHalf(value) : LLVMBuildFPCast(ir_builder, value, ty_target, "");
		return floatToInteger(value, ty_target, destination.unsignedInteger());
	}
	if (destination.floating()) {
		const LLVMTypeRef  ty_compute = destination.width == 16 ? LLVMFloatTypeInContext(LLVMGetModuleContext(mod_module)) : ty_target;
		const LLVMValueRef ir_float   = source.unsignedInteger() ? LLVMBuildUIToFP(ir_builder, value, ty_compute, "") :
		                                                           LLVMBuildSIToFP(ir_builder, value, ty_compute, "");
		return destination.width == 16 ? convertHalf(ir_float) : ir_float;
	}
	return LLVMBuildIntCast2(ir_builder, value, ty_target, source.integer() && !source.unsignedInteger(), "");
}

LLVMValueRef ScalarLowering::checkedCast (LLVMValueRef value, const cloth::ResolvedType &source, const cloth::ExpressionInfo &cast) const {
	const cloth::ResolvedType &rt_target = cast.types.front();
	if (cast.invalidNumericCast) {
		failIf(LLVMConstAllOnes(LLVMInt1TypeInContext(LLVMGetModuleContext(mod_module))), declareInvalidFloatToInteger);
		return LLVMConstNull(type(rt_target));
	}
	if (!std::holds_alternative<std::monostate>(cast.numericConstant)) return constant(rt_target, cast.numericConstant);
	return convert(value, source, rt_target);
}

LLVMValueRef ScalarLowering::integerPower (LLVMValueRef base, LLVMValueRef exponent, bool isUnsigned) const {
	const LLVMTypeRef  ty_integer = LLVMTypeOf(base);
	const LLVMValueRef ir_zero = LLVMConstNull(ty_integer), ir_one = LLVMConstInt(ty_integer, 1, false);
	if (!isUnsigned) failIf(LLVMBuildICmp(ir_builder, LLVMIntSLT, exponent, ir_zero, ""), declareTrap);
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	LLVMBasicBlockRef    bb_start    = LLVMGetInsertBlock(ir_builder);
	const LLVMValueRef   fn_function = LLVMGetBasicBlockParent(bb_start);
	LLVMBasicBlockRef    bb_loop     = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "pow.loop");
	LLVMBasicBlockRef    bb_step     = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "pow.step");
	LLVMBasicBlockRef    bb_end      = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "pow.end");
	LLVMBuildBr(ir_builder, bb_loop);
	LLVMPositionBuilderAtEnd(ir_builder, bb_loop);
	const LLVMValueRef ir_product = LLVMBuildPhi(ir_builder, ty_integer, ""), ir_factor = LLVMBuildPhi(ir_builder, ty_integer, ""),
	                   ir_remaining = LLVMBuildPhi(ir_builder, ty_integer, "");
	LLVMValueRef ir_initial         = ir_one;
	LLVMAddIncoming(ir_product, &ir_initial, &bb_start, 1);
	LLVMAddIncoming(ir_factor, &base, &bb_start, 1);
	LLVMAddIncoming(ir_remaining, &exponent, &bb_start, 1);
	LLVMBuildCondBr(ir_builder, predicate(ir_remaining), bb_step, bb_end);
	LLVMPositionBuilderAtEnd(ir_builder, bb_step);
	// Repeated squaring uses wrapping multiplication at the checked width.
	LLVMValueRef ir_nextProduct = LLVMBuildSelect(ir_builder, predicate(LLVMBuildAnd(ir_builder, ir_remaining, ir_one, "")),
	                                              LLVMBuildMul(ir_builder, ir_product, ir_factor, ""), ir_product, "");
	LLVMValueRef ir_nextFactor  = LLVMBuildMul(ir_builder, ir_factor, ir_factor, "");
	// Shifting an i1 by one is poison. Its only remaining exponent bit has just been consumed.
	LLVMValueRef ir_nextExponent = LLVMGetIntTypeWidth(ty_integer) == 1 ? ir_zero : LLVMBuildLShr(ir_builder, ir_remaining, ir_one, "");
	LLVMAddIncoming(ir_product, &ir_nextProduct, &bb_step, 1);
	LLVMAddIncoming(ir_factor, &ir_nextFactor, &bb_step, 1);
	LLVMAddIncoming(ir_remaining, &ir_nextExponent, &bb_step, 1);
	LLVMBuildBr(ir_builder, bb_loop);
	LLVMPositionBuilderAtEnd(ir_builder, bb_end);
	return ir_product;
}

LLVMValueRef ScalarLowering::binary (cloth::Operator op, LLVMValueRef left, LLVMValueRef right, const cloth::ResolvedType &operandType) const {
	if (operandType.floating() && operandType.width == 16) {
		left  = convertHalf(left);
		right = convertHalf(right);
	}
	if (cloth::isComparison(op)) {
		const LLVMValueRef ir_test = operandType.floating() ?
		                                     LLVMBuildFCmp(ir_builder, floatPredicate(op), left, right, "") :
		                                     LLVMBuildICmp(ir_builder, integerPredicate(op, operandType.unsignedInteger()), left, right, "");
		return boolean(ir_test);
	}
	if (operandType.floating()) {
		// The checker computes half arithmetic in single precision, then rounds the result to half.
		LLVMTypeRef  ty_compute = LLVMTypeOf(left);
		LLVMValueRef ir_result;
		switch (op) {
		case cloth::oPlus:
			ir_result = LLVMBuildFAdd(ir_builder, left, right, "");
			break;
		case cloth::oMinus:
			ir_result = LLVMBuildFSub(ir_builder, left, right, "");
			break;
		case cloth::oMul:
			ir_result = LLVMBuildFMul(ir_builder, left, right, "");
			break;
		case cloth::oDiv:
			ir_result = LLVMBuildFDiv(ir_builder, left, right, "");
			break;
		case cloth::oMod:
			ir_result = LLVMBuildFRem(ir_builder, left, right, "");
			break;
		case cloth::oMulMul: {
			const LLVMValueRef fn_pow        = intrinsic(mod_module, "llvm.pow", &ty_compute, 1);
			LLVMValueRef       v_arguments[] = { left, right };
			ir_result                        = LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn_pow), fn_pow, v_arguments, 2, "");
			break;
		}
		default:
			throw std::logic_error("Invalid primitive floating operation");
		}
		return operandType.width == 16 ? convertHalf(ir_result) : ir_result;
	}
	const bool b_unsigned = operandType.unsignedInteger();
	switch (op) {
	case cloth::oPlus:
		return LLVMBuildAdd(ir_builder, left, right, "");
	case cloth::oMinus:
		return LLVMBuildSub(ir_builder, left, right, "");
	case cloth::oMul:
		return LLVMBuildMul(ir_builder, left, right, "");
	case cloth::oDiv:
		guardDivisor(left, right, b_unsigned);
		return b_unsigned ? LLVMBuildUDiv(ir_builder, left, right, "") : LLVMBuildSDiv(ir_builder, left, right, "");
	case cloth::oMod:
		guardDivisor(left, right, b_unsigned);
		return b_unsigned ? LLVMBuildURem(ir_builder, left, right, "") : LLVMBuildSRem(ir_builder, left, right, "");
	case cloth::oMulMul:
		return integerPower(left, right, b_unsigned);
	default:
		throw std::logic_error("Invalid primitive integer operation");
	}
}
}
