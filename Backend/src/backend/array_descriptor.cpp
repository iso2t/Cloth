// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/array_descriptor.hpp>
#include <backend/nullable_value.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/target_layout.hpp>

namespace backend::detail::array_descriptor {
namespace {

LLVMTypeRef extentType (LLVMTypeRef word) {
	LLVMTypeRef v_fields[] = { word, LLVMInt1TypeInContext(LLVMGetTypeContext(word)) };
	return LLVMStructTypeInContext(LLVMGetTypeContext(word), v_fields, 2, false);
}

LLVMValueRef knownExtent (LLVMTypeRef extent, LLVMValueRef count) {
	LLVMValueRef v_fields[] = { count, LLVMConstAllOnes(LLVMStructGetTypeAtIndex(extent, 1)) };
	return LLVMConstNamedStruct(extent, v_fields, 2);
}

bool hasSharedShape (LLVMTypeRef descriptor) {
	// Only a nullable-row tree has a pointer immediately before capacity and owner.
	return LLVMGetTypeKind(LLVMStructGetTypeAtIndex(descriptor, LLVMCountStructElementTypes(descriptor) - 3)) == LLVMPointerTypeKind;
}
}

LLVMTypeRef type (LLVMModuleRef module, const cloth::CheckedProgram &program, const cloth::ResolvedType &array) {
	const LLVMContextRef     ctx_context = LLVMGetModuleContext(module);
	const LLVMTargetDataRef  td_target   = LLVMGetModuleDataLayout(module);
	const LLVMTypeRef        ty_pointer  = LLVMPointerTypeInContext(ctx_context, 0);
	const LLVMTypeRef        ty_word     = LLVMIntPtrTypeInContext(ctx_context, td_target);
	const LLVMTypeRef        ty_extent   = extentType(ty_word);
	std::vector<LLVMTypeRef> v_fields{ ty_pointer, ty_word };
	bool                     b_sharedShape = false;
	for (cloth::ResolvedType rt_row = array.element(); cloth::semantic::hasArrayShape(rt_row); rt_row = rt_row.element()) {
		if (rt_row.nullable()) {
			b_sharedShape = true;
			rt_row        = rt_row.element();
		}
		v_fields.push_back(ty_extent);
	}
	if (b_sharedShape) v_fields.push_back(ty_pointer);
	v_fields.push_back(ty_word);
	v_fields.push_back(ty_pointer);
	const LLVMTypeRef          ty_descriptor = LLVMStructTypeInContext(ctx_context, v_fields.data(), static_cast<unsigned>(v_fields.size()), false);
	const cloth::StorageLayout sl_actual{ .size      = LLVMABISizeOfType(td_target, ty_descriptor),
		                              .alignment = LLVMABIAlignmentOfType(td_target, ty_descriptor) };
	if (program.storageLayout(array) != sl_actual)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::AbiLayout,
		                           std::format("LLVM array descriptor for '{}' differs from its checked storage layout", array.spelling()));
	return ty_descriptor;
}

LLVMValueRef fixedWitness (LLVMTypeRef descriptorType, std::uint64_t count, LLVMValueRef rowWitness) {
	const LLVMTypeRef         ty_word   = LLVMStructGetTypeAtIndex(descriptorType, 1);
	const LLVMValueRef        ir_null   = LLVMConstNull(LLVMStructGetTypeAtIndex(descriptorType, 0));
	const LLVMValueRef        ir_length = LLVMConstInt(ty_word, count, false);
	std::vector<LLVMValueRef> v_fields{ ir_null, ir_length };
	if (rowWitness) {
		const LLVMTypeRef ty_row      = LLVMTypeOf(rowWitness);
		const unsigned    u_extentEnd = LLVMCountStructElementTypes(ty_row) - (hasSharedShape(ty_row) ? 3u : 2u);
		v_fields.push_back(knownExtent(LLVMStructGetTypeAtIndex(descriptorType, 2), LLVMGetAggregateElement(rowWitness, 1)));
		for (unsigned u_index = 2; u_index < u_extentEnd; ++u_index) v_fields.push_back(LLVMGetAggregateElement(rowWitness, u_index));
	}
	if (hasSharedShape(descriptorType)) v_fields.push_back(ir_null);
	v_fields.push_back(LLVMConstNull(ty_word));
	v_fields.push_back(ir_null);
	return LLVMConstNamedStruct(descriptorType, v_fields.data(), static_cast<unsigned>(v_fields.size()));
}

LLVMValueRef growableWitness (LLVMBuilderRef builder, LLVMValueRef nullableArray, LLVMValueRef previous) {
	const LLVMValueRef descriptor = nullable::payload(builder, nullableArray);
	const LLVMTypeRef  type       = LLVMTypeOf(descriptor);
	const unsigned     end        = LLVMCountStructElementTypes(type) - (hasSharedShape(type) ? 3u : 2u);
	LLVMValueRef       shape      = LLVMConstNull(type);
	// Copy the outer length and inner extents. All allocation and shared-shape pointers stay null.
	for (unsigned i = 1; i < end; ++i) shape = LLVMBuildInsertValue(builder, shape, LLVMBuildExtractValue(builder, descriptor, i, ""), i, "");
	return LLVMBuildSelect(builder, LLVMBuildIsNotNull(builder, nullable::presence(builder, nullableArray), ""), nullable::pack(builder, shape), previous,
	                       "");
}

LLVMValueRef sharedShape (LLVMModuleRef module, LLVMValueRef fixedWitness) {
	const LLVMTypeRef ty_descriptor = LLVMTypeOf(fixedWitness);
	if (!hasSharedShape(ty_descriptor)) return nullptr;
	const LLVMTypeRef         ty_extent = LLVMStructGetTypeAtIndex(ty_descriptor, 2);
	const unsigned            u_shared  = LLVMCountStructElementTypes(ty_descriptor) - 3;
	std::vector<LLVMValueRef> v_bounds{ knownExtent(ty_extent, LLVMGetAggregateElement(fixedWitness, 1)) };
	for (unsigned u_index = 2; u_index < u_shared; ++u_index) v_bounds.push_back(LLVMGetAggregateElement(fixedWitness, u_index));
	std::string s_name = "cloth.array.shape";
	for (const LLVMValueRef ir_bound : v_bounds) s_name += "." + std::to_string(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_bound, 0)));
	if (const LLVMValueRef ir_existing = LLVMGetNamedGlobal(module, s_name.c_str())) return ir_existing;
	const LLVMValueRef ir_bounds = LLVMConstArray2(ty_extent, v_bounds.data(), v_bounds.size());
	const LLVMValueRef ir_shape  = LLVMAddGlobal(module, LLVMTypeOf(ir_bounds), s_name.c_str());
	LLVMSetInitializer(ir_shape, ir_bounds);
	LLVMSetLinkage(ir_shape, LLVMPrivateLinkage);
	LLVMSetGlobalConstant(ir_shape, true);
	LLVMSetUnnamedAddress(ir_shape, LLVMGlobalUnnamedAddr);
	return ir_shape;
}

LLVMValueRef childShape (LLVMModuleRef module, LLVMValueRef parentShape) {
	const LLVMTypeRef ty_word = LLVMIntPtrTypeInContext(LLVMGetModuleContext(module), LLVMGetModuleDataLayout(module));
	LLVMValueRef      ir_one  = LLVMConstInt(ty_word, 1, false);
	return LLVMConstGEP2(extentType(ty_word), parentShape, &ir_one, 1);
}

LLVMValueRef fixed (LLVMBuilderRef builder, LLVMValueRef fixedWitness, LLVMValueRef buffer, LLVMValueRef sharedShape) {
	const LLVMTypeRef ty_descriptor = LLVMTypeOf(fixedWitness);
	const unsigned    u_fields      = LLVMCountStructElementTypes(ty_descriptor);
	LLVMValueRef      ir_header     = LLVMBuildInsertValue(builder, fixedWitness, LLVMGetAggregateElement(fixedWitness, 1), u_fields - 2, "");
	if (hasSharedShape(ty_descriptor)) ir_header = LLVMBuildInsertValue(builder, ir_header, sharedShape, u_fields - 3, "");
	return LLVMBuildInsertValue(builder, ir_header, buffer, 0, "");
}

LLVMValueRef dataAddress (LLVMBuilderRef builder, LLVMValueRef slot) {
	// The data pointer is the first field in descriptors of every rank.
	return LLVMBuildLoad2(builder, LLVMTypeOf(slot), slot, "");
}
}
