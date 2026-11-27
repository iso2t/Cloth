// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/class_objects.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/value_types.hpp>

#include <memory>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <token/token.hpp>

namespace backend::detail {

LLVMValueRef ClassObjects::numberType (const cloth::ResolvedType &primitive, const cloth::ResolvedType &number, const unsigned bits) {
	if (const auto found = m_numberTypes.find(primitive); found != m_numberTypes.end()) return found->second;
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    pointer = LLVMPointerTypeInContext(context, 0), word = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
	LLVMValueRef         equal = LLVMConstNull(pointer);
	if (primitive.floating()) {
		LLVMTypeRef       parameters[] = { pointer, pointer };
		const LLVMTypeRef signature    = LLVMFunctionType(LLVMInt8TypeInContext(context), parameters, 2, false);
		equal                          = LLVMAddFunction(mod_module, "cloth.number.equal", signature);
		LLVMSetLinkage(equal, LLVMInternalLinkage);
		const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> builder(LLVMCreateBuilderInContext(context), LLVMDisposeBuilder);
		LLVMPositionBuilderAtEnd(builder.get(), LLVMAppendBasicBlockInContext(context, equal, "entry"));
		const ScalarLowering scalars(mod_module, builder.get(), cp_program.layout);
		const auto           load = [&] (unsigned index) {
			const LLVMValueRef value = LLVMBuildLoad2(builder.get(), scalars.type(primitive), LLVMGetParam(equal, index), "");
			LLVMSetAlignment(value, 1);
			return value;
		};
		LLVMBuildRet(builder.get(), scalars.binary(cloth::OEqual, load(0), load(1), primitive));
	}
	// Match ClothNumberType in the runtime. The same null-terminated views are used by class type tests.
	LLVMValueRef       views[]    = { typeTag(primitive), typeTag(number), LLVMConstNull(pointer) };
	LLVMValueRef       fields[]   = { LLVMConstArray2(pointer, views, 3), LLVMConstInt(word, (bits + 7) / 8, false), equal };
	const LLVMValueRef descriptor = LLVMConstStructInContext(context, fields, 3, false);
	const LLVMValueRef global     = LLVMAddGlobal(mod_module, LLVMTypeOf(descriptor), "cloth.number.type");
	LLVMSetInitializer(global, descriptor);
	LLVMSetGlobalConstant(global, true);
	LLVMSetLinkage(global, LLVMInternalLinkage);
	return m_numberTypes.emplace(primitive, global).first->second;
}

LLVMValueRef ClassObjects::boxNumber (LLVMBuilderRef builder, const cloth::ResolvedType &primitive, const cloth::ResolvedType &number, LLVMValueRef value,
                                      LLVMValueRef present) {
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	if (primitive == cloth::ResolvedType(cloth::KInteger)) {
		// Real object handles have a nonzero generation and slot. Use 1 through 2^32 for int and leave zero for absence.
		const LLVMTypeRef  handle  = LLVMInt64TypeInContext(context);
		const LLVMValueRef encoded = LLVMBuildAdd(builder, LLVMBuildZExt(builder, value, handle, ""), LLVMConstInt(handle, 1, false), "");
		return present ? LLVMBuildSelect(builder, LLVMBuildIsNotNull(builder, present, ""), encoded, LLVMConstNull(handle), "") : encoded;
	}
	const unsigned bits = primitive.width ? primitive.width : cp_program.layout.sizeBits;
	if (primitive.floating()) value = LLVMBuildBitCast(builder, value, LLVMIntTypeInContext(context, bits), "");
	// Only initialized value bits enter the runtime's canonical representation, including for non-byte integer widths.
	if (bits % 8) value = LLVMBuildZExt(builder, value, LLVMIntTypeInContext(context, (bits + 7) / 8 * 8), "");
	const LLVMValueRef scratch = allocateEntrySlot(builder, LLVMTypeOf(value));
	LLVMBuildStore(builder, value, scratch);
	return objectCall(builder, mod_module, ObjectOperation::NumberBox,
	                  { numberType(primitive, number, bits), scratch, present ? present : LLVMConstInt(LLVMInt8TypeInContext(context), 1, false) });
}

}
