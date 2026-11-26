// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <backend/builtin_lowering.hpp>
#include <backend/nullable_value.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/string_value.hpp>
#include <backend/value_types.hpp>

#include <cloth/text.h>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

#include <ast/decl/enum_declaration.hpp>
#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace backend::detail {
using Op = cloth::BuiltinOperation;

BuiltinLowering::BuiltinLowering (LLVMModuleRef module, LLVMBuilderRef builder, ValueTypes &types, ScalarLowering &scalars) :
        mod_module(module),
        ir_builder(builder),
        vt_types(types),
        sl_scalars(scalars) {
}

LLVMValueRef BuiltinLowering::number (unsigned bits, std::uint64_t value) const {
	return LLVMConstInt(LLVMIntTypeInContext(LLVMGetModuleContext(mod_module), bits), value, false);
}

LLVMValueRef BuiltinLowering::address (LLVMValueRef value) {
	const LLVMValueRef slot = allocateEntrySlot(ir_builder, LLVMTypeOf(value));
	LLVMBuildStore(ir_builder, value, slot);
	return slot;
}

LLVMValueRef BuiltinLowering::invoke (const char *name, LLVMTypeRef result, std::vector<LLVMValueRef> arguments) {
	LLVMValueRef function = LLVMGetNamedFunction(mod_module, name);
	if (!function) {
		std::vector<LLVMTypeRef> parameters;
		parameters.reserve(arguments.size());
		for (const LLVMValueRef value : arguments) parameters.push_back(LLVMTypeOf(value));
		function = LLVMAddFunction(mod_module, name, LLVMFunctionType(result, parameters.data(), static_cast<unsigned>(parameters.size()), false));
	}
	return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(function), function, arguments.data(), static_cast<unsigned>(arguments.size()), "");
}

LLVMValueRef BuiltinLowering::call (const char *name, LLVMTypeRef result, std::initializer_list<LLVMValueRef> arguments) {
	return invoke(name, result, arguments);
}

LLVMValueRef BuiltinLowering::output (const char *name, LLVMTypeRef result, std::initializer_list<LLVMValueRef> arguments) {
	const LLVMValueRef        slot = address(LLVMConstNull(result));
	std::vector<LLVMValueRef> args{ slot };
	args.insert(args.end(), arguments);
	invoke(name, LLVMVoidTypeInContext(LLVMGetModuleContext(mod_module)), std::move(args));
	return LLVMBuildLoad2(ir_builder, result, slot, "");
}

LLVMValueRef BuiltinLowering::optionalText (const char *name, LLVMValueRef argument) {
	const LLVMTypeRef  type    = string_value::type(mod_module);
	const LLVMValueRef slot    = address(LLVMConstNull(type));
	const LLVMValueRef present = call(name, LLVMInt8TypeInContext(LLVMGetModuleContext(mod_module)), { slot, argument });
	return nullable::pack(ir_builder, LLVMBuildLoad2(ir_builder, type, slot, ""), present);
}

void BuiltinLowering::guard (LLVMValueRef failure, const char *message) {
	const LLVMContextRef    context  = LLVMGetModuleContext(mod_module);
	const LLVMValueRef      function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(ir_builder));
	const LLVMBasicBlockRef error    = LLVMAppendBasicBlockInContext(context, function, "invalid");
	const LLVMBasicBlockRef valid    = LLVMAppendBasicBlockInContext(context, function, "valid");
	LLVMBuildCondBr(ir_builder, failure, error, valid);
	LLVMPositionBuilderAtEnd(ir_builder, error);
	call("cloth_rt_error", LLVMVoidTypeInContext(context), { LLVMBuildGlobalStringPtr(ir_builder, message, "") });
	LLVMBuildUnreachable(ir_builder);
	LLVMPositionBuilderAtEnd(ir_builder, valid);
}

LLVMValueRef BuiltinLowering::numeric (Op op, const cloth::ResolvedType &type, LLVMValueRef value, const std::vector<LLVMValueRef> &args) {
	const LLVMContextRef context  = LLVMGetModuleContext(mod_module);
	const bool           floating = type.floating();
	const LLVMTypeRef    stored   = LLVMTypeOf(value);
	const LLVMValueRef   zero     = LLVMConstNull(stored);
	const auto           less     = [&] (LLVMValueRef a, LLVMValueRef b) {
		return floating ? LLVMBuildFCmp(ir_builder, LLVMRealOLT, a, b, "") :
		                  LLVMBuildICmp(ir_builder, type.unsignedInteger() ? LLVMIntULT : LLVMIntSLT, a, b, "");
	};
	switch (op) {
	case Op::Abs:
		if (floating) {
			const unsigned     id       = LLVMLookupIntrinsicID("llvm.fabs", 9);
			LLVMTypeRef        overload = stored;
			const LLVMValueRef fn       = LLVMGetIntrinsicDeclaration(mod_module, id, &overload, 1);
			return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn), fn, &value, 1, "");
		}
		if (type.unsignedInteger()) return value;
		guard(LLVMBuildICmp(ir_builder, LLVMIntEQ, value,
		                    LLVMBuildShl(ir_builder, LLVMConstInt(stored, 1, false), LLVMConstInt(stored, LLVMGetIntTypeWidth(stored) - 1, false), ""),
		                    ""),
		      "absolute value exceeds the signed integer width");
		return LLVMBuildSelect(ir_builder, less(value, zero), LLVMBuildNeg(ir_builder, value, ""), value, "");
	case Op::Clamp: {
		const auto low = args.at(0), high = args.at(1);
		guard(floating ? LLVMBuildFCmp(ir_builder, LLVMRealUGT, low, high, "") : less(high, low), "invalid clamp bounds");
		return LLVMBuildSelect(ir_builder, less(value, low), low, LLVMBuildSelect(ir_builder, less(high, value), high, value, ""), "");
	}
	case Op::Signum:
		if (floating) guard(LLVMBuildFCmp(ir_builder, LLVMRealUNO, value, value, ""), "signum of NaN");
		return LLVMBuildSelect(ir_builder, less(value, zero), number(32, UINT32_MAX),
		                       LLVMBuildZExt(ir_builder, less(zero, value), LLVMInt32TypeInContext(context), ""), "");
	case Op::LeadingZeros:
	case Op::TrailingZeros: {
		if (LLVMGetIntTypeWidth(stored) > 64) {
			return call("cloth_rt_integer_zeros", LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module)),
			            { address(value), number(32, LLVMGetIntTypeWidth(stored)), number(8, op == Op::LeadingZeros) });
		}
		const unsigned     id         = LLVMLookupIntrinsicID(op == Op::LeadingZeros ? "llvm.ctlz" : "llvm.cttz", 9);
		LLVMTypeRef        overload   = stored;
		const LLVMValueRef fn         = LLVMGetIntrinsicDeclaration(mod_module, id, &overload, 1);
		LLVMValueRef       operands[] = { value, number(1, 0) };
		const LLVMValueRef result     = LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn), fn, operands, 2, "");
		return LLVMBuildIntCast2(ir_builder, result, LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module)), false, "");
	}
	case Op::ToStringRadix:
		return output("cloth_rt_text_integer", string_value::type(mod_module),
		              { address(value), number(32, LLVMGetIntTypeWidth(stored)), number(8, !type.unsignedInteger()), args.at(0) });
	default:
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Unrecognized checked numeric method");
	}
}

LLVMValueRef BuiltinLowering::method (const cloth::BuiltinCall &builtin, const cloth::ResolvedType &receiver, LLVMValueRef value,
                                      const std::vector<LLVMValueRef> &args, const cloth::ResolvedType &result) {
	const auto           op      = builtin.operation;
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    byte = LLVMInt8TypeInContext(context), integer = LLVMInt32TypeInContext(context);
	const LLVMTypeRef    word = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
	const LLVMTypeRef    text = string_value::type(mod_module);
	if (op == Op::ToInt) return sl_scalars.convert(value, receiver, result);
	if (receiver.integer() || receiver.floating()) {
		if (receiver.floating() && receiver.width == 16) {
			cloth::ResolvedType wide(receiver);
			wide.width = 32;
			std::vector<LLVMValueRef> bounds;
			bounds.reserve(args.size());
			for (const LLVMValueRef arg : args) bounds.push_back(sl_scalars.convert(arg, receiver, wide));
			const LLVMValueRef answer = numeric(op, wide, sl_scalars.convert(value, receiver, wide), bounds);
			return result.floating() ? sl_scalars.convert(answer, wide, result) : answer;
		}
		return numeric(op, receiver, value, args);
	}
	if (receiver == cloth::ResolvedType(cloth::KCharacter)) {
		int query = CLOTH_WHITESPACE;
		switch (op) {
		case Op::ToUpperCase:
		case Op::ToLowerCase:
			return call("cloth_rt_char_case", integer, { value, number(8, op == Op::ToUpperCase) });
		case Op::IsWhitespace:
			query = CLOTH_WHITESPACE;
			break;
		case Op::IsLetter:
			query = CLOTH_LETTER;
			break;
		case Op::IsDigit:
			query = CLOTH_DIGIT;
			break;
		case Op::IsLetterOrDigit:
			query = CLOTH_LETTER_OR_DIGIT;
			break;
		case Op::IsUpperCase:
			query = CLOTH_UPPER;
			break;
		case Op::IsLowerCase:
			query = CLOTH_LOWER;
			break;
		default:
			throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Unrecognized checked character method");
		}
		return call("cloth_rt_char_test", byte, { value, number(32, query) });
	}
	const LLVMValueRef source = address(value);
	switch (op) {
	case Op::ToUpperCase:
	case Op::ToLowerCase:
		return output("cloth_rt_text_case", text, { source, number(8, op == Op::ToUpperCase) });
	case Op::IsEmpty:
		return sl_scalars.boolean(LLVMBuildIsNull(ir_builder, string_value::bytes(ir_builder, value).length, ""));
	case Op::Contains:
	case Op::IndexOf:
	case Op::LastIndexOf: {
		const LLVMValueRef found   = call("cloth_rt_text_find", word, { source, address(args.at(0)), number(8, op == Op::LastIndexOf) });
		const LLVMValueRef present = sl_scalars.boolean(LLVMBuildICmp(ir_builder, LLVMIntNE, found, LLVMConstAllOnes(word), ""));
		return op == Op::Contains ? present : nullable::pack(ir_builder, found, present);
	}
	case Op::StartsWith:
	case Op::EndsWith:
		return call("cloth_rt_text_matches", byte, { source, address(args.at(0)), number(32, op == Op::StartsWith ? CLOTH_START : CLOTH_END) });
	case Op::CharAt:
		return call("cloth_rt_text_at", integer, { source, args.at(0) });
	case Op::Substring:
		return output("cloth_rt_text_substring", text, { source, args.at(0), args.at(1) });
	case Op::ToChars:
		return output("cloth_rt_text_chars", vt_types.localType(result), { source });
	case Op::Split: {
		LLVMValueRef separator = args.at(0);
		if (builtin.parameters.front() == cloth::ResolvedType(cloth::KCharacter)) separator = formatScalar(builtin.parameters.front(), separator);
		return output("cloth_rt_text_split", vt_types.localType(result), { source, address(separator) });
	}
	case Op::Replace:
	case Op::ReplaceFirst:
		return output("cloth_rt_text_replace", text, { source, address(args.at(0)), address(args.at(1)), number(8, op == Op::ReplaceFirst) });
	case Op::RemovePrefix:
	case Op::RemoveSuffix:
		return output("cloth_rt_text_affix", text, { source, address(args.at(0)), number(32, op == Op::RemovePrefix ? CLOTH_START : CLOTH_END) });
	case Op::IsBlank:
	case Op::Trim:
	case Op::TrimStart:
	case Op::TrimEnd: {
		const LLVMValueRef trimmed = output("cloth_rt_text_trim", text,
		                                    { source, number(32, op == Op::TrimStart ? CLOTH_START :
		                                                         op == Op::TrimEnd   ? CLOTH_END :
		                                                                               CLOTH_BOTH) });
		return op == Op::IsBlank ? sl_scalars.boolean(LLVMBuildIsNull(ir_builder, string_value::bytes(ir_builder, trimmed).length, "")) : trimmed;
	}
	default:
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Unrecognized checked text method");
	}
}

LLVMValueRef BuiltinLowering::equalText (LLVMValueRef left, LLVMValueRef right, bool optional) {
	const auto content = [&] (LLVMValueRef value) {
		if (!optional) return value;
		return LLVMBuildSelect(ir_builder, sl_scalars.predicate(nullable::presence(ir_builder, value)), nullable::payload(ir_builder, value),
		                       LLVMConstNull(string_value::type(mod_module)), "");
	};
	const LLVMValueRef same      = call("cloth_rt_text_matches", LLVMInt8TypeInContext(LLVMGetModuleContext(mod_module)),
	                                    { address(content(left)), address(content(right)), number(32, CLOTH_BOTH) });
	const LLVMValueRef predicate = sl_scalars.predicate(same);
	return optional ? nullable::equal(ir_builder, left, right, predicate) : predicate;
}

LLVMValueRef BuiltinLowering::enumName (const cloth::ResolvedType &type, LLVMValueRef tag) {
	std::vector<LLVMValueRef> names;
	for (const auto &entry : cloth::semantic::enumeration(type)->cases) names.push_back(string_value::constant(mod_module, entry.name.getLexeme()));
	const LLVMTypeRef  text   = string_value::type(mod_module);
	const LLVMValueRef table  = LLVMConstArray2(text, names.data(), names.size());
	const LLVMValueRef global = LLVMAddGlobal(mod_module, LLVMTypeOf(table), "cloth.enum.names");
	LLVMSetInitializer(global, table);
	LLVMSetGlobalConstant(global, true);
	LLVMSetLinkage(global, LLVMPrivateLinkage);
	LLVMValueRef indices[] = { number(32, 0), tag };
	return LLVMBuildLoad2(ir_builder, text, LLVMBuildGEP2(ir_builder, LLVMTypeOf(table), global, indices, 2, ""), "");
}

LLVMValueRef BuiltinLowering::formatScalar (const cloth::ResolvedType &type, LLVMValueRef value) {
	const LLVMTypeRef text = string_value::type(mod_module);
	if (type == cloth::ResolvedType(cloth::KString)) return value;
	if (type.integer()) return numeric(Op::ToStringRadix, type, value, { number(32, 10) });
	if (type.floating()) {
		auto wide  = type;
		wide.width = 64;
		return output("cloth_rt_text_float", text, { sl_scalars.convert(value, type, wide), number(32, type.width) });
	}
	if (type == cloth::ResolvedType(cloth::KBoolean))
		return LLVMBuildSelect(ir_builder, sl_scalars.predicate(value), string_value::constant(mod_module, cloth::toString(cloth::KTrue)),
		                       string_value::constant(mod_module, cloth::toString(cloth::KFalse)), "");
	if (type == cloth::ResolvedType(cloth::KCharacter)) return output("cloth_rt_text_char", text, { value });
	if (type.enumeration()) return enumName(type, value);
	if (type == cloth::ResolvedType(cloth::KNull)) return string_value::constant(mod_module, cloth::toString(cloth::KNull));
	return string_value::constant(mod_module, type.spelling());
}

LLVMValueRef BuiltinLowering::join (std::span<const LLVMValueRef> parts) {
	const LLVMTypeRef text = string_value::type(mod_module);
	if (parts.empty()) return LLVMConstNull(text);
	if (parts.size() == 1) return parts.front();
	LLVMValueRef array = LLVMConstNull(LLVMArrayType2(text, parts.size()));
	for (unsigned i = 0; i < parts.size(); ++i) array = LLVMBuildInsertValue(ir_builder, array, parts[i], i, "");
	const LLVMTypeRef word = LLVMIntPtrTypeInContext(LLVMGetModuleContext(mod_module), LLVMGetModuleDataLayout(mod_module));
	return output("cloth_rt_text_join", text, { address(array), LLVMConstInt(word, parts.size(), false) });
}
}
