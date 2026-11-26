// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst
// See Documentation/compiler/internals/Backend.rst

#include <backend/growable_array.hpp>
#include <backend/runtime.hpp>
#include <backend/value_types.hpp>

#include <cstdint>
#include <span>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/target_layout.hpp>
#include <token/token.hpp>

namespace backend::detail::growable_array {
namespace {
constexpr unsigned DATA = 0, LENGTH = 1, CAPACITY = 2;

LLVMValueRef word (LLVMModuleRef module, std::uint64_t value) {
	return LLVMConstInt(LLVMIntPtrTypeInContext(LLVMGetModuleContext(module), LLVMGetModuleDataLayout(module)), value, false);
}

void call (LLVMBuilderRef builder, LLVMValueRef function, std::span<LLVMValueRef> arguments) {
	LLVMBuildCall2(builder, LLVMGlobalGetValueType(function), function, arguments.data(), static_cast<unsigned>(arguments.size()), "");
}
}

bool supported (const cloth::ResolvedType &type) {
	if (!type.array() || type.arrayExtent()) return false;
	const cloth::ResolvedType  rt_cell     = type.element();
	const cloth::ResolvedType  rt_value    = rt_cell.nullable() ? rt_cell.element() : rt_cell;
	const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(rt_value);
	return rt_value.integer() || rt_value.floating() || rt_value.enumeration() || rt_value.objectReference() ||
	       rt_value == cloth::ResolvedType(cloth::KBoolean) || rt_value == cloth::ResolvedType(cloth::KCharacter) ||
	       rt_value == cloth::ResolvedType(cloth::KString) || (rt_value.suffixes.empty() && cloth::semantic::asStruct(rt_value.declaration)) ||
	       (rt_endpoint && rt_endpoint->integer());
}

bool ownsBuffer (const cloth::ResolvedType &type) {
	return supported(type.nullable() ? type.element() : type);
}

LLVMValueRef borrow (LLVMBuilderRef builder, LLVMTypeRef descriptorType, LLVMValueRef data, LLVMValueRef length) {
	LLVMValueRef ir_value = LLVMBuildInsertValue(builder, LLVMConstNull(descriptorType), data, DATA, "");
	ir_value              = LLVMBuildInsertValue(builder, ir_value, length, LENGTH, "");
	return LLVMBuildInsertValue(builder, ir_value, length, CAPACITY, "");
}

Elements elements (LLVMBuilderRef builder, LLVMValueRef descriptor) {
	return { .data = LLVMBuildExtractValue(builder, descriptor, DATA, ""), .length = LLVMBuildExtractValue(builder, descriptor, LENGTH, "") };
}

void assign (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef data, LLVMValueRef count, cloth::StorageLayout cell) {
	LLVMValueRef v_arguments[] = { destination, data, count, word(module, cell.size), word(module, cell.alignment) };
	call(builder, declareArrayAssign(module), v_arguments);
}

void reserve (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef minimum, cloth::StorageLayout cell) {
	LLVMValueRef v_arguments[] = { destination, minimum, word(module, cell.size), word(module, cell.alignment) };
	call(builder, declareArrayReserve(module), v_arguments);
}

void append (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef cellAddress, cloth::StorageLayout cell) {
	LLVMValueRef v_arguments[] = { destination, cellAddress, word(module, cell.size), word(module, cell.alignment) };
	call(builder, declareArrayAppend(module), v_arguments);
}

void insert (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef index, LLVMValueRef cellAddress, cloth::StorageLayout cell) {
	LLVMValueRef v_arguments[] = { destination, index, cellAddress, word(module, cell.size), word(module, cell.alignment) };
	call(builder, declareArrayInsert(module), v_arguments);
}

LLVMValueRef removeAt (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination, LLVMValueRef index, LLVMTypeRef cellType,
                       cloth::StorageLayout cell) {
	const LLVMValueRef ir_removed    = allocateEntrySlot(builder, cellType);
	LLVMValueRef       v_arguments[] = { destination, index, ir_removed, word(module, cell.size), word(module, cell.alignment) };
	call(builder, declareArrayRemoveAt(module), v_arguments);
	return LLVMBuildLoad2(builder, cellType, ir_removed, "");
}

void clear (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination) {
	LLVMValueRef v_arguments[] = { destination };
	call(builder, declareArrayClear(module), v_arguments);
}

void release (LLVMBuilderRef builder, LLVMModuleRef module, LLVMValueRef destination) {
	LLVMValueRef v_arguments[] = { destination };
	call(builder, declareArrayRelease(module), v_arguments);
}
}
