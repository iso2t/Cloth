// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/value_types.hpp>

#include <format>
#include <memory>

#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/target_layout.hpp>

namespace backend::detail {

cloth::StorageLayout ValueTypes::cellLayout (const cloth::ResolvedType &element) {
	const auto        checked = cp_program.storageLayout(element);
	const LLVMTypeRef cell    = slotType(element);
	if (!checked || !cell)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "LLVM lowering requires a checked storage layout for the array element");
	const LLVMTargetDataRef    target = LLVMGetModuleDataLayout(mod_module);
	const cloth::StorageLayout actual{ .size = LLVMABISizeOfType(target, cell), .alignment = LLVMABIAlignmentOfType(target, cell) };
	if (actual != *checked)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::AbiLayout,
		                           std::format("LLVM array cell for '{}' differs from its checked storage layout", element.spelling()));
	return *checked;
}

void ValueTypes::initializeCell (LLVMBuilderRef builder, const cloth::ResolvedType &element, LLVMValueRef slot, LLVMValueRef value) {
	if (ownsBuffers(element)) value = cloneBuffers(builder, element, value);
	ArrayPath path{ .dimensions = {}, .heapCell = true };
	storeValue(builder, element, slot, value, StoragePlace::Slot, &path);
}

LLVMValueRef ValueTypes::prepareCell (LLVMBuilderRef builder, const cloth::ResolvedType &element, LLVMValueRef value) {
	const LLVMValueRef slot = allocateEntrySlot(builder, slotType(element));
	initializeCell(builder, element, slot, value);
	return slot;
}

LLVMValueRef ValueTypes::takeCell (LLVMBuilderRef builder, const cloth::ResolvedType &element, LLVMValueRef slot) {
	const LLVMValueRef value = loadLocal(builder, element, slot);
	// Fixed fields have been gathered into the value. Its growable descriptors now own the removed cell's buffers.
	if (!flatCell(element)) visitBuffers(builder, element, slot, nullptr, StoragePlace::Slot, CellCleanup::Transfer);
	return value;
}

LLVMValueRef ValueTypes::cellOperations (const cloth::ResolvedType &element) {
	if (flatCell(element)) return nullptr;
	if (const auto found = m_cellOperations.find(element); found != m_cellOperations.end()) return found->second;
	const LLVMContextRef context      = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    ptr          = LLVMPointerTypeInContext(context, 0);
	const LLVMTypeRef    word         = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
	const LLVMTypeRef    nothing      = LLVMVoidTypeInContext(context);
	LLVMTypeRef          parameters[] = { ptr, ptr };
	const LLVMValueRef   copy         = LLVMAddFunction(mod_module, "cloth.copy.cell", LLVMFunctionType(nothing, parameters, 2, false));
	const LLVMValueRef   dispose      = LLVMAddFunction(mod_module, "cloth.dispose.cell", LLVMFunctionType(nothing, parameters, 1, false));
	LLVMSetLinkage(copy, LLVMInternalLinkage);
	LLVMSetLinkage(dispose, LLVMInternalLinkage);
	const auto         layout     = cellLayout(element);
	LLVMValueRef       fields[]   = { LLVMConstInt(word, layout.size, false), LLVMConstInt(word, layout.alignment, false), copy, dispose };
	const LLVMValueRef table      = LLVMConstStructInContext(context, fields, 4, false);
	const LLVMValueRef operations = LLVMAddGlobal(mod_module, LLVMTypeOf(table), "cloth.cell.operations");
	LLVMSetInitializer(operations, table);
	LLVMSetLinkage(operations, LLVMPrivateLinkage);
	LLVMSetGlobalConstant(operations, true);
	m_cellOperations.emplace(element, operations);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> builder(LLVMCreateBuilderInContext(context), LLVMDisposeBuilder);
	LLVMPositionBuilderAtEnd(builder.get(), LLVMAppendBasicBlockInContext(context, copy, "entry"));
	initializeCell(builder.get(), element, LLVMGetParam(copy, 0), loadLocal(builder.get(), element, LLVMGetParam(copy, 1)));
	LLVMBuildRetVoid(builder.get());
	LLVMPositionBuilderAtEnd(builder.get(), LLVMAppendBasicBlockInContext(context, dispose, "entry"));
	visitBuffers(builder.get(), element, LLVMGetParam(dispose, 0), nullptr, StoragePlace::Slot, CellCleanup::Dispose);
	LLVMBuildRetVoid(builder.get());
	return operations;
}
}
