// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/growable_array.hpp>
#include <backend/rectangular_array.hpp>
#include <backend/runtime.hpp>

#include <cstddef>
#include <initializer_list>
#include <vector>

#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>

namespace backend::detail::rectangular_array {

cloth::ResolvedType leafType (cloth::ResolvedType type) {
	while (type.array()) type.suffixes.pop_back();
	return type;
}

cloth::ResolvedType openType (cloth::ResolvedType type) {
	const auto leaf = leafType(type);
	for (std::size_t i = leaf.suffixes.size(); i < type.suffixes.size(); ++i) type.suffixes[i] = cloth::semantic::arraySuffix();
	return type;
}

bool supported (const cloth::ResolvedType &type) {
	if (!type.array() || !type.element().array()) return false;
	auto row  = type;
	bool open = false;
	while (row.array()) {
		open |= !row.arrayExtent().has_value();
		row.suffixes.pop_back();
	}
	row.suffixes.push_back(cloth::semantic::arraySuffix());
	return open && growable_array::supported(row);
}

LLVMValueRef shape (LLVMTypeRef descriptor, cloth::ResolvedType array) {
	LLVMValueRef              value = LLVMConstNull(descriptor);
	const LLVMTypeRef         word  = LLVMStructGetTypeAtIndex(descriptor, 1);
	std::vector<LLVMValueRef> fields(LLVMCountStructElementTypes(descriptor));
	for (unsigned i = 0; i < fields.size(); ++i) fields[i] = LLVMGetAggregateElement(value, i);
	if (const auto count = array.arrayExtent()) fields[1] = LLVMConstInt(word, *count, false);
	for (unsigned i = 2; (array = array.element()).array(); ++i) {
		if (const auto count = array.arrayExtent()) {
			const LLVMTypeRef extent    = LLVMStructGetTypeAtIndex(descriptor, i);
			LLVMValueRef      members[] = { LLVMConstInt(word, *count, false), LLVMConstAllOnes(LLVMStructGetTypeAtIndex(extent, 1)) };
			fields[i]                   = LLVMConstNamedStruct(extent, members, 2);
		}
	}
	return LLVMConstNamedStruct(descriptor, fields.data(), static_cast<unsigned>(fields.size()));
}

Layout layout (LLVMModuleRef module, const cloth::CheckedProgram &program, const cloth::ResolvedType &type) {
	const auto leaf = leafType(type);
	const auto cell = program.storageLayout(leaf);
	if (!cell) throw cde::ClothDiagnostic(cde::DiagnosticCode::AbiLayout, "Missing rectangular array cell layout");
	const LLVMTypeRef word = LLVMIntPtrTypeInContext(LLVMGetModuleContext(module), LLVMGetModuleDataLayout(module));
	return { .rank      = LLVMConstInt(word, type.suffixes.size() - leaf.suffixes.size(), false),
		 .stride    = LLVMConstInt(word, cell->size, false),
		 .alignment = LLVMConstInt(word, cell->alignment, false) };
}

void call (LLVMBuilderRef builder, LLVMModuleRef module, const char *name, std::initializer_list<LLVMValueRef> arguments) {
	callRuntime(builder, module, name, LLVMVoidTypeInContext(LLVMGetModuleContext(module)), arguments);
}
}
