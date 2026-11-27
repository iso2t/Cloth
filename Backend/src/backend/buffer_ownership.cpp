// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/array_descriptor.hpp>
#include <backend/growable_array.hpp>
#include <backend/rectangular_array.hpp>
#include <backend/runtime.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <memory>
#include <tuple>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

bool ValueTypes::flatCell (const cloth::ResolvedType &type) const {
	if (type.nullable()) return flatCell(type.element());
	if (type.array()) return false;
	const auto layout = cp_program.structLayouts.find(type);
	return layout == cp_program.structLayouts.end() || std::ranges::all_of(layout->second.fields, [&] (const auto &field) { return flatCell(field.type); });
}

bool ValueTypes::ownsBuffers (const cloth::ResolvedType &type) const {
	if (type.nullable()) return ownsBuffers(type.element());
	if (type.array()) return growable_array::ownsBuffer(type) || (type.arrayExtent() && ownsBuffers(type.element()));
	const auto layout = cp_program.structLayouts.find(type);
	return layout != cp_program.structLayouts.end() &&
	       std::ranges::any_of(layout->second.fields, [&] (const auto &field) { return ownsBuffers(field.type); });
}

void ValueTypes::visitBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef target, LLVMValueRef source, StoragePlace place, CellCleanup cleanup) {
	if (cleanup == CellCleanup::Transfer && growable_array::ownsBuffer(type)) return;
	const LLVMTypeRef stored = storageType(type, place);
	if (rectangular_array::supported(type)) {
		const auto layout = rectangular_array::layout(mod_module, cp_program, type);
		if (!source) return rectangular_array::call(builder, mod_module, "cloth_rt_rows_release", { target, layout.rank });
		LLVMBuildStore(builder, LLVMConstNull(stored), target);
		return rectangular_array::call(builder, mod_module, "cloth_rt_rows_assign", { target, source, layout.rank, layout.stride, layout.alignment });
	}
	if (growable_array::supported(type)) {
		const LLVMValueRef operations = cellOperations(type.element());
		if (!source) return growable_array::release(builder, mod_module, target, operations);
		const auto elements = growable_array::elements(builder, LLVMBuildLoad2(builder, stored, source, ""));
		LLVMBuildStore(builder, LLVMConstNull(stored), target);
		return growable_array::assign(builder, mod_module, target, elements.data, elements.length, cellLayout(type.element()), operations);
	}
	const auto field = [&] (const cloth::ResolvedType &member, unsigned index, StoragePlace memberPlace) {
		if (cleanup != CellCleanup::None ? !flatCell(member) : ownsBuffers(member))
			visitBuffers(builder, member, LLVMBuildStructGEP2(builder, stored, target, index, ""),
			             source ? LLVMBuildStructGEP2(builder, stored, source, index, "") : nullptr, memberPlace, cleanup);
	};
	if (type.nullable()) return field(type.element(), 1, place);
	if (const auto count = type.arrayExtent()) {
		if (*count && !flatCell(type.element())) visitFixedCells(builder, type, target, source, place, cleanup);
		if (cleanup != CellCleanup::None && place == StoragePlace::Embedded) {
			const LLVMValueRef ownerSlot = LLVMBuildStructGEP2(builder, stored, target, LLVMCountStructElementTypes(stored) - 1, "");
			callRuntime(builder, mod_module, "cloth_rt_cell_free", LLVMVoidTypeInContext(LLVMGetModuleContext(mod_module)),
			            { LLVMBuildLoad2(builder, LLVMTypeOf(target), ownerSlot, "") });
		}
		return;
	}
	unsigned index = 0;
	for (const auto &member : cp_program.structLayouts.at(type).fields)
		field(member.type, index++, place == StoragePlace::Value ? place : StoragePlace::Embedded);
}

void ValueTypes::visitFixedCells (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef target, LLVMValueRef source, StoragePlace place,
                                  CellCleanup cleanup) {
	const auto              element  = type.element();
	const LLVMTypeRef       cells    = place == StoragePlace::Value ? localType(type) : slotType(type);
	const LLVMContextRef    context  = LLVMGetModuleContext(mod_module);
	const LLVMBasicBlockRef start    = LLVMGetInsertBlock(builder);
	const LLVMValueRef      function = LLVMGetBasicBlockParent(start);
	const LLVMBasicBlockRef body     = LLVMAppendBasicBlockInContext(context, function, "cells");
	const LLVMBasicBlockRef done     = LLVMAppendBasicBlockInContext(context, function, "done");
	const LLVMTypeRef       word     = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
	if (place == StoragePlace::Embedded) {
		// Cleanup can reach a zeroed field whose initializer was skipped.
		target = array_descriptor::dataAddress(builder, target);
		LLVMBuildCondBr(builder, LLVMBuildIsNotNull(builder, target, ""), body, done);
	} else {
		LLVMBuildBr(builder, body);
	}
	LLVMPositionBuilderAtEnd(builder, body);
	const LLVMValueRef index = LLVMBuildPhi(builder, word, "");
	LLVMValueRef       zero  = LLVMConstNull(word);
	LLVMBasicBlockRef  from  = start;
	LLVMAddIncoming(index, &zero, &from, 1);
	LLVMValueRef indices[] = { zero, index };
	visitBuffers(builder, element, LLVMBuildGEP2(builder, cells, target, indices, 2, ""),
	             source ? LLVMBuildGEP2(builder, cells, source, indices, 2, "") : nullptr,
	             place == StoragePlace::Value ? place : arrayElementPlace(element), cleanup);
	LLVMValueRef next = LLVMBuildAdd(builder, index, LLVMConstInt(word, 1, false), "");
	from              = LLVMGetInsertBlock(builder);
	LLVMAddIncoming(index, &next, &from, 1);
	LLVMBuildCondBr(builder, LLVMBuildICmp(builder, LLVMIntULT, next, LLVMConstInt(word, LLVMGetArrayLength2(cells), false), ""), body, done);
	LLVMPositionBuilderAtEnd(builder, done);
}

LLVMValueRef ValueTypes::bufferOperation (const cloth::ResolvedType &type, bool copy, StoragePlace place) {
	const auto key = std::make_tuple(type, copy, place);
	if (const auto found = m_bufferOperations.find(key); found != m_bufferOperations.end()) return found->second;
	const LLVMContextRef context  = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    pointer  = LLVMPointerTypeInContext(context, 0);
	LLVMTypeRef          params[] = { pointer, pointer };
	const LLVMValueRef   function = LLVMAddFunction(mod_module, copy ? "cloth.copy.buffers" : "cloth.dispose.buffers",
	                                                LLVMFunctionType(LLVMVoidTypeInContext(context), params, copy ? 2 : 1, false));
	LLVMSetLinkage(function, LLVMInternalLinkage);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> builder(LLVMCreateBuilderInContext(context), LLVMDisposeBuilder);
	LLVMPositionBuilderAtEnd(builder.get(), LLVMAppendBasicBlockInContext(context, function, "entry"));
	const LLVMValueRef target = LLVMGetParam(function, 0);
	const LLVMValueRef source = copy ? LLVMGetParam(function, 1) : nullptr;
	if (copy) LLVMBuildStore(builder.get(), LLVMBuildLoad2(builder.get(), localType(type), source, ""), target);
	visitBuffers(builder.get(), type, target, source, place);
	LLVMBuildRetVoid(builder.get());
	m_bufferOperations.emplace(key, function);
	return function;
}

LLVMValueRef ValueTypes::bufferDisposer (const cloth::ResolvedType &type, StoragePlace place) {
	if (place != StoragePlace::Value && storageType(type, place) == localType(type)) place = StoragePlace::Value;
	return bufferOperation(type, false, place);
}

LLVMValueRef ValueTypes::cloneBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef value) {
	const LLVMTypeRef  stored = localType(type);
	const LLVMValueRef result = allocateEntrySlot(builder, stored);
	const LLVMValueRef input  = allocateEntrySlot(builder, stored);
	LLVMBuildStore(builder, value, input);
	LLVMValueRef       args[] = { result, input };
	const LLVMValueRef copy   = bufferOperation(type, true, StoragePlace::Value);
	LLVMBuildCall2(builder, LLVMGlobalGetValueType(copy), copy, args, 2, "");
	return LLVMBuildLoad2(builder, stored, result, "");
}

LLVMValueRef ValueTypes::replaceBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value, StoragePlace place) {
	// Clone the value before releasing a possibly aliased destination. The caller installs the clone in the destination's layout.
	const LLVMValueRef result  = cloneBuffers(builder, type, value);
	const LLVMValueRef dispose = bufferDisposer(type, place);
	LLVMBuildCall2(builder, LLVMGlobalGetValueType(dispose), dispose, &slot, 1, "");
	return result;
}
}
