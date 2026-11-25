// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/array_descriptor.hpp>
#include <backend/growable_array.hpp>
#include <backend/integer_range.hpp>
#include <backend/nullable_value.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/string_value.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/struct_storage.hpp>
#include <semantic/target_layout.hpp>
#include <token/token.hpp>

namespace backend::detail {

StoragePlace arrayElementPlace (const cloth::ResolvedType &element) {
	return element.nullable() && cloth::semantic::hasArrayShape(element) ? StoragePlace::Embedded : StoragePlace::Slot;
}

LLVMValueRef allocateEntrySlot (LLVMBuilderRef builder, LLVMTypeRef type, LLVMValueRef initial) {
	const LLVMBasicBlockRef bb_site  = LLVMGetInsertBlock(builder);
	const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(LLVMGetBasicBlockParent(bb_site));
	if (const LLVMValueRef ir_first = LLVMGetFirstInstruction(bb_entry))
		LLVMPositionBuilderBefore(builder, ir_first);
	else
		LLVMPositionBuilderAtEnd(builder, bb_entry);
	const LLVMValueRef ir_slot = LLVMBuildAlloca(builder, type, "");
	if (initial) LLVMBuildStore(builder, initial, ir_slot);
	LLVMPositionBuilderAtEnd(builder, bb_site);
	return ir_slot;
}

ValueTypes::ValueTypes (LLVMModuleRef module, const cloth::CheckedProgram &program) : mod_module(module), cp_program(program) {
}

std::optional<cloth::StorageLayout> ValueTypes::aggregateLayout (const std::vector<LLVMTypeRef> &fields) const {
	const LLVMTargetDataRef target = LLVMGetModuleDataLayout(mod_module);
	cloth::StructStorage    layout(cp_program.layout);
	for (const LLVMTypeRef field : fields) {
		if (!layout.append({ .size = LLVMABISizeOfType(target, field), .alignment = LLVMABIAlignmentOfType(target, field) })) return {};
	}
	return layout.finish(false);
}

LLVMTypeRef ValueTypes::nullableType (LLVMTypeRef payload) const {
	if (!payload) return nullptr;
	const LLVMTargetDataRef td_target   = LLVMGetModuleDataLayout(mod_module);
	const std::uint64_t     u64_bytes   = LLVMABISizeOfType(td_target, payload);
	const unsigned          u_alignment = LLVMABIAlignmentOfType(td_target, payload);
	if (u64_bytes > cp_program.layout.maximumSize() - u_alignment) return nullptr;
	const LLVMTypeRef ty_nullable = nullable::type(payload);
	return LLVMABISizeOfType(td_target, ty_nullable) == u64_bytes + u_alignment ? ty_nullable : nullptr;
}

LLVMTypeRef ValueTypes::localType (const cloth::ResolvedType &type) {
	if (growable_array::supported(type)) return array_descriptor::type(mod_module, cp_program, type);
	if (type.nullable()) {
		if (growable_array::supported(type.element())) return nullableType(localType(type.element()));
		return slotType(type) ? nullableType(localType(type.element())) : nullptr;
	}
	if (type.array()) {
		const ArrayTypes *at_types = arrayTypes(type);
		return at_types ? at_types->value : nullptr;
	}
	if (cp_program.structLayouts.contains(type)) {
		const StructTypes *st_types = structTypes(type);
		return st_types ? st_types->value : nullptr;
	}
	return get(type);
}

LLVMTypeRef ValueTypes::slotType (const cloth::ResolvedType &type) {
	if (type.nullable()) return nullableType(slotType(type.element()));
	if (!type.array()) return get(type);
	const ArrayTypes *at_types = arrayTypes(type);
	return at_types ? at_types->buffer : nullptr;
}

std::uint64_t ValueTypes::backingBytes (const cloth::ResolvedType &field) {
	if (field.nullable()) return backingBytes(field.element());
	if (field.array()) {
		const ArrayTypes &at_types = *arrayTypes(field);
		return std::max<std::uint64_t>(at_types.backingBytes, LLVMABISizeOfType(LLVMGetModuleDataLayout(mod_module), at_types.buffer));
	}
	const StructTypes *st_types = structTypes(field);
	return st_types ? st_types->backingBytes : 0;
}

const ArrayTypes *ValueTypes::arrayTypes (const cloth::ResolvedType &type) {
	if (const auto it_type = m_arrays.find(type); it_type != m_arrays.end()) return &it_type->second;
	const std::optional<std::uint64_t> opt_count = type.arrayExtent();
	if (!opt_count) return nullptr;
	const cloth::ResolvedType rt_element = type.element();
	const LLVMTypeRef         ty_stored  = arrayElementPlace(rt_element) == StoragePlace::Embedded ? get(rt_element) : slotType(rt_element);
	if (!ty_stored) return nullptr;
	const LLVMTypeRef ty_element = localType(rt_element);
	if (!ty_element) return nullptr;
	const std::optional<cloth::StorageLayout> opt_data = cp_program.arrayDataLayout(type, *opt_count);
	if (!opt_data) return nullptr;
	const LLVMTargetDataRef td_target      = LLVMGetModuleDataLayout(mod_module);
	const std::uint64_t     u64_valueBytes = LLVMABISizeOfType(td_target, ty_element);
	// Each field path owns a separate block across all outer dimensions. Its physical cells can be larger than the SSA snapshot.
	const std::uint64_t u64_backing = rt_element.array() ? arrayTypes(rt_element)->backingBytes : backingBytes(rt_element);
	const std::uint64_t u64_largest = std::max(u64_backing, u64_valueBytes);
	if (u64_largest && *opt_count > cp_program.layout.maximumSize() / u64_largest) return nullptr;
	const LLVMTypeRef          ty_buffer = LLVMArrayType2(ty_stored, *opt_count);
	const cloth::StorageLayout sl_actual{ .size = LLVMABISizeOfType(td_target, ty_buffer), .alignment = LLVMABIAlignmentOfType(td_target, ty_buffer) };
	if (sl_actual != *opt_data)
		throw cde::ClothDiagnostic(
		        cde::DiagnosticCode::AbiLayout,
		        std::format("LLVM gives the data buffer for '{}' size {} and alignment {}. The checked layout requires size {} and alignment {}",
		                    type.spelling(), sl_actual.size, sl_actual.alignment, opt_data->size, opt_data->alignment));
	const LLVMTypeRef ty_value = ty_stored == ty_element ? ty_buffer : LLVMArrayType2(ty_element, *opt_count);
	if (LLVMABISizeOfType(td_target, ty_value) != u64_valueBytes * *opt_count) return nullptr;
	const LLVMTypeRef         ty_descriptor = array_descriptor::type(mod_module, cp_program, type);
	const cloth::ResolvedType rt_row        = rt_element.nullable() ? rt_element.element() : rt_element;
	const LLVMValueRef        ir_rowShape   = rt_row.array() ? arrayTypes(rt_row)->fixedShape : nullptr;
	return &m_arrays.emplace(type, ArrayTypes{ .buffer       = ty_buffer,
	                                           .value        = ty_value,
	                                           .descriptor   = ty_descriptor,
	                                           .fixedShape   = array_descriptor::fixedWitness(ty_descriptor, *opt_count, ir_rowShape),
	                                           .backingBytes = u64_backing * *opt_count })
	                .first->second;
}

LLVMTypeRef ValueTypes::get (const cloth::ResolvedType &type) {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
	if (type.nullable()) return nullableType(get(type.element()));
	if (type.array()) {
		const ArrayTypes *at_types = arrayTypes(type);
		return at_types ? at_types->descriptor : nullptr;
	}
	if (const LLVMTypeRef ty_scalar = scalarType(ctx_context, cp_program.layout, type)) return ty_scalar;
	if (type == cloth::ResolvedType(cloth::KString)) return string_value::type(mod_module);
	if (const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(type); rt_endpoint && rt_endpoint->integer()) {
		const LLVMTypeRef ty_endpoint = get(*rt_endpoint);
		return ty_endpoint ? IntegerRange::type(ty_endpoint) : nullptr;
	}
	// An untyped null carries no data until a surrounding expression supplies the nullable type.
	if (type == cloth::ResolvedType(cloth::KNull)) return LLVMStructTypeInContext(ctx_context, nullptr, 0, false);
	const StructTypes *st_types = structTypes(type);
	return st_types ? st_types->storage : nullptr;
}

const StructTypes *ValueTypes::structTypes (const cloth::ResolvedType &type) {
	const auto it_layout = cp_program.structLayouts.find(type);
	if (it_layout == cp_program.structLayouts.end()) return nullptr;
	const cloth::StructLayout &sl_layout = it_layout->second;
	if (const auto it_type = m_structs.find(&sl_layout); it_type != m_structs.end()) return &it_type->second;

	const LLVMContextRef         ctx_context = LLVMGetModuleContext(mod_module);
	std::map<unsigned, unsigned> m_witnesses;
	std::vector<LLVMTypeRef>     v_elements;
	std::vector<LLVMTypeRef>     v_values;
	std::uint64_t                u64_backing = 0;
	v_elements.reserve(sl_layout.fields.size());
	v_values.reserve(sl_layout.fields.size());
	for (const cloth::StructLayout::Field &field : sl_layout.fields) {
		const LLVMTypeRef ty_field = get(field.type);
		if (!ty_field) return nullptr;
		const LLVMTypeRef ty_value = localType(field.type);
		if (!ty_value) return nullptr;
		u64_backing = std::max(u64_backing, backingBytes(field.type));
		if (field.type.nullable() && field.type.element().array())
			m_witnesses.emplace(static_cast<unsigned>(v_elements.size()), static_cast<unsigned>(sl_layout.fields.size() + m_witnesses.size()));
		v_elements.push_back(ty_field);
		v_values.push_back(ty_value);
	}
	// Witnesses follow the declared fields. A fixed shape has only one varying fact: whether the field has ever been present.
	for (const auto &[u_field, u_witness] : m_witnesses) {
		v_elements.push_back(v_elements[u_field]);
		v_values.push_back(LLVMInt8TypeInContext(ctx_context));
	}
	// Cloth reserves one byte for an empty struct. LLVM's empty struct occupies zero bytes.
	if (v_elements.empty()) {
		v_elements.push_back(LLVMInt8TypeInContext(ctx_context));
		v_values = v_elements;
	}
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
	LLVMTypeRef ty_value = ty_struct;
	if (v_elements != v_values) {
		// Individually representable buffers can overflow when their cells are captured in one SSA value.
		const std::optional<cloth::StorageLayout> opt_value = aggregateLayout(v_values);
		if (!opt_value) return nullptr;
		ty_value = LLVMStructTypeInContext(ctx_context, v_values.data(), static_cast<unsigned>(v_values.size()), false);
		if (LLVMABISizeOfType(td_target, ty_value) != opt_value->size) return nullptr;
	}
	return &m_structs
	                .emplace(&sl_layout,
	                         StructTypes{ .storage = ty_struct, .value = ty_value, .witnesses = std::move(m_witnesses), .backingBytes = u64_backing })
	                .first->second;
}
}
