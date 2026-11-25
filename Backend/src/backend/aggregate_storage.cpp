// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/array_descriptor.hpp>
#include <backend/nullable_value.hpp>
#include <backend/value_types.hpp>

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

LLVMValueRef ValueTypes::arrayFieldWitness (LLVMBuilderRef builder, const cloth::ResolvedType &owner, LLVMValueRef parentAddress, unsigned fieldIndex) {
	const StructTypes &st_types   = *structTypes(owner);
	const auto         it_witness = st_types.witnesses.find(fieldIndex);
	return it_witness == st_types.witnesses.end() ? nullptr : LLVMBuildStructGEP2(builder, st_types.storage, parentAddress, it_witness->second, "");
}

LLVMValueRef ValueTypes::arrayData (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot) {
	if (type.nullable()) slot = nullable::payloadAddress(builder, get(type.element()), slot);
	return array_descriptor::dataAddress(builder, slot);
}

LLVMValueRef ValueTypes::allocateLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef value) {
	const LLVMValueRef ir_slot = allocateEntrySlot(builder, slotType(type));
	ArrayPath          v_path;
	storeValue(builder, type, ir_slot, value, StoragePlace::Slot, &v_path);
	return ir_slot;
}

void ValueTypes::storeLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value, StoragePlace place,
                             LLVMValueRef witness) {
	if (witness) {
		// Clearing a single field keeps its previous shape. Replacing its entire containing struct copies the source's witness instead.
		storeWitness(builder, type, witness, value, nullable::loadPresence(builder, witness));
	}
	storeValue(builder, type, address, value, place, nullptr);
}

void ValueTypes::initializeField (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value, LLVMValueRef self) {
	ArrayPath path{ .dimensions = {}, .owner = self, .origin = LLVMGetInsertBlock(builder) };
	storeValue(builder, type, slot, value, StoragePlace::Slot, &path);
}

LLVMValueRef ValueTypes::loadLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, StoragePlace place) {
	const LLVMTypeRef ty_value  = localType(type);
	const LLVMTypeRef ty_stored = place == StoragePlace::Embedded ? get(type) : slotType(type);
	if (ty_stored == ty_value) return LLVMBuildLoad2(builder, ty_value, address, "");
	if (type.array()) {
		const LLVMValueRef ir_buffer = place == StoragePlace::Embedded ? array_descriptor::dataAddress(builder, address) : address;
		if (slotType(type) == ty_value) return LLVMBuildLoad2(builder, ty_value, ir_buffer, "");
		const LLVMValueRef ir_snapshot = allocateEntrySlot(builder, ty_value);
		copyBuffer(builder, type, ir_buffer, ir_snapshot, Transfer::Load, nullptr);
		return LLVMBuildLoad2(builder, ty_value, ir_snapshot, "");
	}
	if (type.nullable()) {
		const cloth::ResolvedType rt_payload = type.element();
		const LLVMValueRef        ir_payload =
		        loadLocal(builder, rt_payload, nullable::payloadAddress(builder, LLVMStructGetTypeAtIndex(ty_stored, 1), address), place);
		return nullable::pack(builder, ir_payload, nullable::loadPresence(builder, address));
	}
	const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(type);
	LLVMValueRef               ir_value  = LLVMConstNull(ty_value);
	for (unsigned u_index = 0; const cloth::StructLayout::Field &field : sl_layout.fields) {
		const LLVMValueRef ir_field = LLVMBuildStructGEP2(builder, ty_stored, address, u_index, "");
		ir_value = LLVMBuildInsertValue(builder, ir_value, loadLocal(builder, field.type, ir_field, StoragePlace::Embedded), u_index, "");
		++u_index;
	}
	for (const auto &[u_field, u_witness] : structTypes(type)->witnesses) {
		const LLVMValueRef ir_witness = LLVMBuildStructGEP2(builder, ty_stored, address, u_witness, "");
		ir_value                      = LLVMBuildInsertValue(builder, ir_value, nullable::loadPresence(builder, ir_witness), u_witness, "");
	}
	return ir_value;
}

void ValueTypes::storeWitness (LLVMBuilderRef builder, const cloth::ResolvedType &fieldType, LLVMValueRef address, LLVMValueRef value,
                               LLVMValueRef remembered) {
	const LLVMValueRef        ir_known = LLVMBuildOr(builder, nullable::presence(builder, value), remembered, "");
	const cloth::ResolvedType rt_array = fieldType.element();
	const LLVMValueRef        ir_shape = nullable::presentConstant(arrayTypes(rt_array)->fixedShape);
	const LLVMValueRef ir_witness = LLVMBuildSelect(builder, LLVMBuildIsNotNull(builder, ir_known, ""), ir_shape, LLVMConstNull(LLVMTypeOf(ir_shape)), "");
	LLVMBuildStore(builder, ir_witness, address);
}

void ValueTypes::storeValue (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value, StoragePlace place,
                             ArrayPath *initialization, LLVMValueRef sharedShape) {
	const LLVMTypeRef ty_stored = place == StoragePlace::Embedded ? get(type) : slotType(type);
	if (ty_stored == LLVMTypeOf(value)) {
		LLVMBuildStore(builder, value, address);
		return;
	}
	if (type.array()) {
		const ArrayTypes &at_types  = *arrayTypes(type);
		const LLVMTypeRef ty_buffer = at_types.buffer;
		LLVMValueRef      ir_buffer = address;
		if (place == StoragePlace::Embedded) {
			ir_buffer = initialization ? allocateBacking(builder, ty_buffer, *initialization) : array_descriptor::dataAddress(builder, address);
			if (initialization) {
				if (!sharedShape) sharedShape = array_descriptor::sharedShape(mod_module, at_types.fixedShape);
				LLVMBuildStore(builder, array_descriptor::fixed(builder, at_types.fixedShape, ir_buffer, sharedShape), address);
			}
		}
		if (ty_buffer == LLVMTypeOf(value)) {
			LLVMBuildStore(builder, value, ir_buffer);
		} else {
			const LLVMValueRef ir_snapshot = allocateEntrySlot(builder, LLVMTypeOf(value));
			LLVMBuildStore(builder, value, ir_snapshot);
			copyBuffer(builder, type, ir_buffer, ir_snapshot, Transfer::Store, initialization, sharedShape);
		}
		return;
	}
	if (type.nullable()) {
		// The payload's buffers belong to the slot and remain initialized even when its tag is cleared.
		LLVMBuildStore(builder, nullable::presence(builder, value), address);
		const cloth::ResolvedType rt_payload = type.element();
		storeValue(builder, rt_payload, nullable::payloadAddress(builder, LLVMStructGetTypeAtIndex(ty_stored, 1), address),
		           nullable::payload(builder, value), place, initialization, sharedShape);
		return;
	}
	const cloth::StructLayout &sl_layout = cp_program.structLayouts.at(type);
	for (unsigned u_index = 0; const cloth::StructLayout::Field &field : sl_layout.fields) {
		const LLVMValueRef ir_field = LLVMBuildStructGEP2(builder, ty_stored, address, u_index, "");
		storeValue(builder, field.type, ir_field, LLVMBuildExtractValue(builder, value, u_index, ""), StoragePlace::Embedded, initialization);
		++u_index;
	}
	for (const auto &[u_field, u_witness] : structTypes(type)->witnesses) {
		const LLVMValueRef ir_address = LLVMBuildStructGEP2(builder, ty_stored, address, u_witness, "");
		storeWitness(builder, sl_layout.fields[u_field].type, ir_address, LLVMBuildExtractValue(builder, value, u_field, ""),
		             LLVMBuildExtractValue(builder, value, u_witness, ""));
	}
}
}
