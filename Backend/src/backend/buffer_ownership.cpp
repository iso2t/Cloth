// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/growable_array.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <memory>
#include <utility>

#include <diagnostics.hpp>
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
	if (type.array()) return type.arrayExtent() ? ownsBuffers(type.element()) : growable_array::supported(type);
	const auto layout = cp_program.structLayouts.find(type);
	return layout != cp_program.structLayouts.end() &&
	       std::ranges::any_of(layout->second.fields, [&] (const auto &field) { return ownsBuffers(field.type); });
}

void ValueTypes::visitBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef target, LLVMValueRef source) {
	const LLVMTypeRef stored = localType(type);
	if (growable_array::supported(type)) {
		if (!source) return growable_array::release(builder, mod_module, target);
		const auto elements = growable_array::elements(builder, LLVMBuildLoad2(builder, stored, source, ""));
		LLVMBuildStore(builder, LLVMConstNull(stored), target);
		const auto layout = cp_program.storageLayout(type.element());
		if (!layout) throw cde::ClothDiagnostic(cde::DiagnosticCode::AbiLayout, "Missing checked array cell layout");
		return growable_array::assign(builder, mod_module, target, elements.data, elements.length, *layout);
	}
	const auto field = [&] (const cloth::ResolvedType &member, unsigned index) {
		if (ownsBuffers(member))
			visitBuffers(builder, member, LLVMBuildStructGEP2(builder, stored, target, index, ""),
			             source ? LLVMBuildStructGEP2(builder, stored, source, index, "") : nullptr);
	};
	if (type.nullable()) return field(type.element(), 1);
	if (const auto count = type.arrayExtent()) {
		if (!*count) return;
		const LLVMContextRef    context  = LLVMGetModuleContext(mod_module);
		const LLVMBasicBlockRef start    = LLVMGetInsertBlock(builder);
		const LLVMValueRef      function = LLVMGetBasicBlockParent(start);
		const LLVMBasicBlockRef body     = LLVMAppendBasicBlockInContext(context, function, "cells");
		const LLVMBasicBlockRef done     = LLVMAppendBasicBlockInContext(context, function, "done");
		const LLVMTypeRef       word     = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module));
		LLVMBuildBr(builder, body);
		LLVMPositionBuilderAtEnd(builder, body);
		const LLVMValueRef index = LLVMBuildPhi(builder, word, "");
		LLVMValueRef       zero  = LLVMConstNull(word);
		LLVMBasicBlockRef  from  = start;
		LLVMAddIncoming(index, &zero, &from, 1);
		LLVMValueRef indices[] = { zero, index };
		visitBuffers(builder, type.element(), LLVMBuildGEP2(builder, stored, target, indices, 2, ""),
		             source ? LLVMBuildGEP2(builder, stored, source, indices, 2, "") : nullptr);
		LLVMValueRef next = LLVMBuildAdd(builder, index, LLVMConstInt(word, 1, false), "");
		from              = LLVMGetInsertBlock(builder);
		LLVMAddIncoming(index, &next, &from, 1);
		LLVMBuildCondBr(builder, LLVMBuildICmp(builder, LLVMIntULT, next, LLVMConstInt(word, *count, false), ""), body, done);
		LLVMPositionBuilderAtEnd(builder, done);
		return;
	}
	unsigned index = 0;
	for (const auto &member : cp_program.structLayouts.at(type).fields) field(member.type, index++);
}

LLVMValueRef ValueTypes::bufferOperation (const cloth::ResolvedType &type, bool copy) {
	const auto key = std::make_pair(type, copy);
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
	visitBuffers(builder.get(), type, target, source);
	LLVMBuildRetVoid(builder.get());
	m_bufferOperations.emplace(key, function);
	return function;
}

LLVMValueRef ValueTypes::bufferDisposer (const cloth::ResolvedType &type) {
	return bufferOperation(type, false);
}

void ValueTypes::copyBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value) {
	// Capture all source buffers before releasing any destination that could alias them.
	const LLVMTypeRef  stored = localType(type);
	const LLVMValueRef result = allocateEntrySlot(builder, stored);
	const LLVMValueRef input  = allocateEntrySlot(builder, stored);
	LLVMBuildStore(builder, value, input);
	LLVMValueRef       args[] = { result, input };
	const LLVMValueRef copy   = bufferOperation(type, true);
	LLVMBuildCall2(builder, LLVMGlobalGetValueType(copy), copy, args, 2, "");
	const LLVMValueRef dispose = bufferDisposer(type);
	LLVMBuildCall2(builder, LLVMGlobalGetValueType(dispose), dispose, &slot, 1, "");
	LLVMBuildStore(builder, LLVMBuildLoad2(builder, stored, result, ""), slot);
}
}
