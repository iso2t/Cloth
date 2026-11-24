// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/integer_range.hpp>
#include <backend/nullable_value.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/value_types.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <vector>

#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/target_layout.hpp>
#include <token/token.hpp>

namespace backend::detail {

LLVMValueRef allocateEntrySlot (LLVMBuilderRef builder, LLVMTypeRef type) {
	const LLVMBasicBlockRef bb_site  = LLVMGetInsertBlock(builder);
	const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(LLVMGetBasicBlockParent(bb_site));
	if (const LLVMValueRef ir_first = LLVMGetFirstInstruction(bb_entry))
		LLVMPositionBuilderBefore(builder, ir_first);
	else
		LLVMPositionBuilderAtEnd(builder, bb_entry);
	const LLVMValueRef ir_slot = LLVMBuildAlloca(builder, type, "");
	LLVMPositionBuilderAtEnd(builder, bb_site);
	return ir_slot;
}

ValueTypes::ValueTypes (LLVMModuleRef module, const cloth::CheckedProgram &program) : mod_module(module), cp_program(program) {
}

LLVMTypeRef ValueTypes::directCallType (const cloth::ResolvedType &type) const {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	if (type.nullable()) {
		const LLVMTypeRef ty_payload = scalarType(ctx_context, cp_program.layout, type.element());
		return ty_payload ? nullable::type(ty_payload) : nullptr;
	}
	return scalarType(ctx_context, cp_program.layout, type);
}

LLVMTypeRef ValueTypes::localType (const cloth::ResolvedType &type) {
	if (!type.array()) return get(type);
	const std::optional<std::uint64_t> opt_count = type.arrayExtent();
	if (!opt_count) return nullptr;
	const LLVMTypeRef                         ty_element = localType(type.element());
	const std::optional<cloth::StorageLayout> opt_data   = cp_program.arrayDataLayout(type, *opt_count);
	if (!ty_element || !opt_data) return nullptr;
	const LLVMTypeRef          ty_buffer = LLVMArrayType2(ty_element, *opt_count);
	const LLVMTargetDataRef    td_target = LLVMGetModuleDataLayout(mod_module);
	const cloth::StorageLayout sl_actual{ .size = LLVMABISizeOfType(td_target, ty_buffer), .alignment = LLVMABIAlignmentOfType(td_target, ty_buffer) };
	if (sl_actual != *opt_data)
		throw cde::ClothDiagnostic(
		        cde::DiagnosticCode::AbiLayout,
		        std::format("LLVM gives the data buffer for '{}' size {} and alignment {}. The checked layout requires size {} and alignment {}",
		                    type.spelling(), sl_actual.size, sl_actual.alignment, opt_data->size, opt_data->alignment));
	return ty_buffer;
}

LLVMTypeRef ValueTypes::get (const cloth::ResolvedType &type) {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	if (type.nullable()) {
		const LLVMTypeRef ty_payload = get(type.element());
		return ty_payload ? nullable::type(ty_payload) : nullptr;
	}
	if (const LLVMTypeRef ty_scalar = scalarType(ctx_context, cp_program.layout, type)) return ty_scalar;
	if (const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(type); rt_endpoint && rt_endpoint->integer()) {
		const LLVMTypeRef ty_endpoint = get(*rt_endpoint);
		return ty_endpoint ? IntegerRange::type(ty_endpoint) : nullptr;
	}
	// An untyped null carries no data until a surrounding expression supplies the nullable type.
	if (type == cloth::ResolvedType(cloth::KNull)) return LLVMStructTypeInContext(ctx_context, nullptr, 0, false);
	const auto it_layout = cp_program.structLayouts.find(type);
	if (it_layout == cp_program.structLayouts.end()) return nullptr;
	const cloth::StructLayout &sl_layout = it_layout->second;
	if (const auto it_type = m_structs.find(&sl_layout); it_type != m_structs.end()) return it_type->second;

	std::vector<LLVMTypeRef> v_elements;
	v_elements.reserve(sl_layout.fields.size());
	// The frontend has checked that inline struct storage is acyclic.
	for (const cloth::StructLayout::Field &field : sl_layout.fields) {
		const LLVMTypeRef ty_field = get(field.type);
		if (!ty_field) return nullptr;
		v_elements.push_back(ty_field);
	}
	// Cloth reserves one byte for an empty struct. LLVM's empty struct occupies zero bytes.
	if (v_elements.empty()) v_elements.push_back(LLVMInt8TypeInContext(ctx_context));
	const LLVMTypeRef          ty_struct = LLVMStructTypeInContext(ctx_context, v_elements.data(), static_cast<unsigned>(v_elements.size()), false);
	const LLVMTargetDataRef    td_target = LLVMGetModuleDataLayout(mod_module);
	const cloth::StorageLayout sl_actual{ .size = LLVMABISizeOfType(td_target, ty_struct), .alignment = LLVMABIAlignmentOfType(td_target, ty_struct) };
	if (sl_actual != sl_layout.storage) {
		throw cde::ClothDiagnostic(cde::DiagnosticCode::AbiLayout,
		                           std::format("LLVM gives '{}' size {} and alignment {}. The checked layout requires size {} and alignment {}",
		                                       type.spelling(), sl_actual.size, sl_actual.alignment, sl_layout.storage.size,
		                                       sl_layout.storage.alignment),
		                           { { .span = type.declaration->name.getSpan(), .text = "struct layout mismatch" } });
	}
	for (std::size_t st_index = 0; st_index < sl_layout.fields.size(); ++st_index) {
		const cloth::Symbol &sym_field    = *sl_layout.fields[st_index].symbol;
		const std::uint64_t  u64_actual   = LLVMOffsetOfElement(td_target, ty_struct, static_cast<unsigned>(st_index));
		const std::uint64_t  u64_expected = sl_layout.fieldOffsets.at(&sym_field);
		if (u64_actual != u64_expected) {
			throw cde::ClothDiagnostic(cde::DiagnosticCode::AbiLayout,
			                           std::format("LLVM places field '{}' at byte {}. The checked layout requires byte {}",
			                                       sym_field.name.getLexeme(), u64_actual, u64_expected),
			                           { { .span = sym_field.name.getSpan(), .text = "field offset mismatch" } });
		}
	}
	m_structs.emplace(&sl_layout, ty_struct);
	return ty_struct;
}
}
