// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <backend/builtin_lowering.hpp>
#include <backend/growable_array.hpp>
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
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace backend::detail {
BuiltinLowering::BuiltinLowering (LLVMModuleRef module, LLVMBuilderRef builder, ScalarLowering &scalars) :
        mod_module(module),
        ir_builder(builder),
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

LLVMValueRef BuiltinLowering::floatAbs (LLVMValueRef value) {
	const unsigned     id       = LLVMLookupIntrinsicID("llvm.fabs", 9);
	LLVMTypeRef        overload = LLVMTypeOf(value);
	const LLVMValueRef fn       = LLVMGetIntrinsicDeclaration(mod_module, id, &overload, 1);
	return LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn), fn, &value, 1, "");
}

LLVMValueRef BuiltinLowering::integerZeros (LLVMValueRef value, bool leading) {
	LLVMTypeRef       stored = LLVMTypeOf(value);
	const unsigned    width  = LLVMGetIntTypeWidth(stored);
	const LLVMTypeRef size   = LLVMIntPtrTypeInContext(LLVMGetModuleContext(mod_module), LLVMGetModuleDataLayout(mod_module));
	if (width > 64) return call("cloth_rt_integer_zeros", size, { address(value), number(32, width), number(8, leading) });
	const unsigned     id         = LLVMLookupIntrinsicID(leading ? "llvm.ctlz" : "llvm.cttz", 9);
	const LLVMValueRef fn         = LLVMGetIntrinsicDeclaration(mod_module, id, &stored, 1);
	LLVMValueRef       operands[] = { value, number(1, 0) };
	const LLVMValueRef result     = LLVMBuildCall2(ir_builder, LLVMGlobalGetValueType(fn), fn, operands, 2, "");
	return LLVMBuildIntCast2(ir_builder, result, size, false, "");
}

LLVMValueRef BuiltinLowering::integerText (const cloth::ResolvedType &type, LLVMValueRef value, LLVMValueRef radix) {
	return output("cloth_rt_text_integer", string_value::type(mod_module),
	              { address(value), number(32, LLVMGetIntTypeWidth(LLVMTypeOf(value))), number(8, !type.unsignedInteger()), radix });
}

LLVMValueRef BuiltinLowering::textIntrinsic (cloth::LibraryIntrinsic operation, LLVMTypeRef result, std::span<const LLVMValueRef> operands) {
	using Intrinsic  = cloth::LibraryIntrinsic;
	const auto query = [&] (int property) { return call("cloth_rt_char_test", result, { operands[0], number(32, property) }); };
	switch (operation) {
	case Intrinsic::CharCodePoint:
		return operands[0];
	case Intrinsic::CharFormat:
		return output("cloth_rt_text_char", result, { operands[0] });
	case Intrinsic::CharWhitespace:
		return query(CLOTH_WHITESPACE);
	case Intrinsic::CharLetter:
		return query(CLOTH_LETTER);
	case Intrinsic::CharDigit:
		return query(CLOTH_DIGIT);
	case Intrinsic::CharUpper:
		return query(CLOTH_UPPER);
	case Intrinsic::CharLower:
		return query(CLOTH_LOWER);
	case Intrinsic::CharMapCase:
		return call("cloth_rt_char_case", result, { operands[0], operands[1] });
	case Intrinsic::StringFind: {
		const auto word  = LLVMIntPtrTypeInContext(LLVMGetModuleContext(mod_module), LLVMGetModuleDataLayout(mod_module));
		const auto index = call("cloth_rt_text_find", word, { address(operands[0]), address(operands[1]), operands[2] });
		return nullable::pack(ir_builder, index, sl_scalars.boolean(LLVMBuildICmp(ir_builder, LLVMIntNE, index, LLVMConstAllOnes(word), "")));
	}
	case Intrinsic::StringMatchesAffix: {
		const auto end = LLVMBuildSelect(ir_builder, sl_scalars.predicate(operands[2]), number(32, CLOTH_END), number(32, CLOTH_START), "");
		return call("cloth_rt_text_matches", result, { address(operands[0]), address(operands[1]), end });
	}
	case Intrinsic::StringCharAt:
		return call("cloth_rt_text_at", result, { address(operands[0]), operands[1] });
	case Intrinsic::StringSubstring:
		return output("cloth_rt_text_substring", result, { address(operands[0]), operands[1], operands[2] });
	case Intrinsic::StringToChars:
		return output("cloth_rt_text_chars", result, { address(operands[0]) });
	case Intrinsic::StringSplit:
		return output("cloth_rt_text_split", result, { address(operands[0]), address(operands[1]) });
	case Intrinsic::StringEncodeChars:
	case Intrinsic::StringConcatParts: {
		const auto elements = growable_array::elements(ir_builder, operands[0]);
		return output(operation == Intrinsic::StringEncodeChars ? "cloth_rt_text_from_chars" : "cloth_rt_text_join", result,
		              { elements.data, elements.length });
	}
	default:
		return nullptr;
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

LLVMValueRef BuiltinLowering::floatText (const cloth::ResolvedType &type, LLVMValueRef value) {
	auto wide  = type;
	wide.width = 64;
	return output("cloth_rt_text_float", string_value::type(mod_module), { sl_scalars.convert(value, type, wide), number(32, type.width) });
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
