// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/array_descriptor.hpp>
#include <backend/value_types.hpp>

#include <cstdint>
#include <ranges>
#include <vector>

#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

LLVMValueRef ValueTypes::allocateBacking (LLVMBuilderRef builder, LLVMTypeRef bufferType, const ArrayPath &path) const {
	// One allocation covers this field at every coordinate of its enclosing arrays.
	LLVMTypeRef             ty_block  = bufferType;
	const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(mod_module);
	std::uint64_t           u64_bytes = LLVMABISizeOfType(td_target, bufferType);
	for (const ArrayDimension &dimension : std::views::reverse(path)) {
		// arrayTypes has checked the product of the enclosing extents and the largest backing for this field path.
		u64_bytes *= dimension.count;
		ty_block = LLVMArrayType2(ty_block, dimension.count);
	}
	if (LLVMABISizeOfType(td_target, ty_block) != u64_bytes)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "LLVM cannot represent the fixed-array field buffer on this target");
	const LLVMValueRef ir_block = allocateEntrySlot(builder, ty_block);
	if (path.empty()) return ir_block;
	std::vector<LLVMValueRef> v_indices{ LLVMConstNull(LLVMTypeOf(path.front().index)) };
	for (const ArrayDimension &dimension : path) v_indices.push_back(dimension.index);
	return LLVMBuildGEP2(builder, ty_block, ir_block, v_indices.data(), static_cast<unsigned>(v_indices.size()), "");
}

void ValueTypes::copyBuffer (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef buffer, LLVMValueRef snapshot, Transfer direction,
                             ArrayPath *initialization, LLVMValueRef sharedShape) {
	const LLVMTypeRef ty_buffer = slotType(type);
	// An empty inner dimension also removes every leaf, even when the outer extent is large.
	if (!LLVMABISizeOfType(LLVMGetModuleDataLayout(mod_module), ty_buffer)) return;
	const std::uint64_t     u64_count   = LLVMGetArrayLength2(ty_buffer);
	const LLVMContextRef    ctx_context = LLVMGetModuleContext(mod_module);
	LLVMBasicBlockRef       bb_before   = LLVMGetInsertBlock(builder);
	const LLVMValueRef      fn_function = LLVMGetBasicBlockParent(bb_before);
	const LLVMBasicBlockRef bb_body     = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "array.copy.cell");
	const LLVMBasicBlockRef bb_done     = LLVMAppendBasicBlockInContext(ctx_context, fn_function, "array.copy.done");
	LLVMBuildBr(builder, bb_body);
	LLVMPositionBuilderAtEnd(builder, bb_body);
	const LLVMTypeRef  ty_index = LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(mod_module));
	LLVMValueRef       ir_zero  = LLVMConstNull(ty_index);
	const LLVMValueRef ir_index = LLVMBuildPhi(builder, ty_index, "");
	LLVMAddIncoming(ir_index, &ir_zero, &bb_before, 1);
	LLVMValueRef              v_indices[] = { ir_zero, ir_index };
	const LLVMValueRef        ir_cell     = LLVMBuildGEP2(builder, ty_buffer, buffer, v_indices, 2, "");
	const LLVMValueRef        ir_value    = LLVMBuildGEP2(builder, localType(type), snapshot, v_indices, 2, "");
	const cloth::ResolvedType rt_element  = type.element();
	const StoragePlace        sp_element  = arrayElementPlace(rt_element);
	const LLVMValueRef ir_childShape = sharedShape && cloth::semantic::hasArrayShape(rt_element) ? array_descriptor::childShape(mod_module, sharedShape) :
	                                                                                               nullptr;
	if (initialization) initialization->push_back({ .count = u64_count, .index = ir_index });
	if (rt_element.array()) {
		copyBuffer(builder, rt_element, ir_cell, ir_value, direction, initialization, ir_childShape);
	} else if (direction == Transfer::Store) {
		storeValue(builder, rt_element, ir_cell, LLVMBuildLoad2(builder, localType(rt_element), ir_value, ""), sp_element, initialization,
		           ir_childShape);
	} else {
		LLVMBuildStore(builder, loadLocal(builder, rt_element, ir_cell, sp_element), ir_value);
	}
	if (initialization) initialization->pop_back();
	// A nested conversion can finish in its own exit block. Its value feeds this loop's back edge.
	LLVMBasicBlockRef bb_end  = LLVMGetInsertBlock(builder);
	LLVMValueRef      ir_next = LLVMBuildAdd(builder, ir_index, LLVMConstInt(ty_index, 1, false), "");
	LLVMAddIncoming(ir_index, &ir_next, &bb_end, 1);
	const LLVMValueRef ir_more = LLVMBuildICmp(builder, LLVMIntULT, ir_next, LLVMConstInt(ty_index, u64_count, false), "");
	LLVMBuildCondBr(builder, ir_more, bb_body, bb_done);
	LLVMPositionBuilderAtEnd(builder, bb_done);
}
}
