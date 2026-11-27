// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <backend/array_descriptor.hpp>
#include <backend/growable_array.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>
#include <backend/value_types.hpp>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

LLVMValueRef ValueTypes::insertStructField (LLVMBuilderRef builder, const cloth::ResolvedType &owner, LLVMValueRef aggregate, unsigned index,
                                            LLVMValueRef value) {
	aggregate             = LLVMBuildInsertValue(builder, aggregate, value, index, "");
	const auto &witnesses = structTypes(owner)->witnesses;
	if (const auto witness = witnesses.find(index); witness != witnesses.end()) {
		const auto        &field = cp_program.structLayouts.at(owner).fields[index].type;
		const LLVMValueRef shape = growable_array::ownsBuffer(field) ?
		                                   array_descriptor::growableWitness(builder, value, LLVMConstNull(LLVMTypeOf(value))) :
		                                   nullable::presence(builder, value);
		aggregate                = LLVMBuildInsertValue(builder, aggregate, shape, witness->second, "");
	}
	return aggregate;
}

LLVMValueRef ValueTypes::arrayFieldWitness (LLVMBuilderRef builder, const cloth::ResolvedType &owner, LLVMValueRef parentAddress, unsigned fieldIndex) {
	const StructTypes &st_types   = *structTypes(owner);
	const auto         it_witness = st_types.witnesses.find(fieldIndex);
	return it_witness == st_types.witnesses.end() ? nullptr : LLVMBuildStructGEP2(builder, st_types.storage, parentAddress, it_witness->second, "");
}

void ValueTypes::updateGrowableWitness (LLVMBuilderRef builder, const cloth::ResolvedType &fieldType, LLVMValueRef address, LLVMValueRef witness) {
	const LLVMTypeRef  type     = get(fieldType);
	const LLVMValueRef field    = LLVMBuildLoad2(builder, type, address, "");
	const LLVMValueRef previous = LLVMBuildLoad2(builder, type, witness, "");
	LLVMBuildStore(builder, array_descriptor::growableWitness(builder, field, previous), witness);
}

LLVMValueRef ValueTypes::arrayData (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot) {
	if (type.nullable()) slot = nullable::payloadAddress(builder, get(type.element()), slot);
	return array_descriptor::dataAddress(builder, slot);
}

LLVMValueRef ValueTypes::allocateLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef value) {
	const LLVMTypeRef  stored  = slotType(type);
	const LLVMValueRef ir_slot = allocateEntrySlot(builder, stored, ownsBuffers(type) ? LLVMConstNull(stored) : nullptr);
	initializeSlot(builder, type, ir_slot, value);
	return ir_slot;
}

void ValueTypes::storeLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value, StoragePlace place,
                             LLVMValueRef witness) {
	if (ownsBuffers(type)) value = replaceBuffers(builder, type, address, value, place);
	storeValue(builder, type, address, value, place, nullptr);
	if (witness) {
		// Clearing a single field keeps its previous shape. Replacing its entire containing struct copies the source's witness instead.
		if (growable_array::ownsBuffer(type))
			updateGrowableWitness(builder, type, address, witness);
		else
			storeFixedWitness(builder, type, witness, value, nullable::loadPresence(builder, witness));
	}
}

void ValueTypes::initializeSlot (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value, LLVMValueRef self) {
	if (ownsBuffers(type)) value = replaceBuffers(builder, type, slot, value, StoragePlace::Slot);
	ArrayPath path{ .dimensions = {}, .owner = self, .origin = LLVMGetInsertBlock(builder) };
	storeValue(builder, type, slot, value, StoragePlace::Slot, &path);
}

LLVMValueRef ValueTypes::loadLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, StoragePlace place) {
	const LLVMTypeRef ty_value  = localType(type);
	const LLVMTypeRef ty_stored = storageType(type, place);
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
		const LLVMValueRef ir_witness  = LLVMBuildStructGEP2(builder, ty_stored, address, u_witness, "");
		const LLVMTypeRef  witnessType = LLVMStructGetTypeAtIndex(ty_value, u_witness);
		ir_value                       = LLVMBuildInsertValue(builder, ir_value, LLVMBuildLoad2(builder, witnessType, ir_witness, ""), u_witness, "");
	}
	return ir_value;
}

void ValueTypes::storeFixedWitness (LLVMBuilderRef builder, const cloth::ResolvedType &fieldType, LLVMValueRef address, LLVMValueRef value,
                               LLVMValueRef remembered) {
	const LLVMValueRef        ir_known = LLVMBuildOr(builder, nullable::presence(builder, value), remembered, "");
	const cloth::ResolvedType rt_array = fieldType.element();
	const LLVMValueRef        ir_shape = nullable::presentConstant(arrayTypes(rt_array)->fixedShape);
	const LLVMValueRef ir_witness = LLVMBuildSelect(builder, LLVMBuildIsNotNull(builder, ir_known, ""), ir_shape, LLVMConstNull(LLVMTypeOf(ir_shape)), "");
	LLVMBuildStore(builder, ir_witness, address);
}

void ValueTypes::storeValue (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value, StoragePlace place,
                             ArrayPath *initialization, LLVMValueRef sharedShape) {
	const LLVMTypeRef ty_stored = storageType(type, place);
	if (ty_stored == LLVMTypeOf(value)) {
		LLVMBuildStore(builder, value, address);
		return;
	}
	if (type.array()) {
		const ArrayTypes &at_types  = *arrayTypes(type);
		const LLVMTypeRef ty_buffer = at_types.buffer;
		LLVMValueRef      ir_buffer = address;
		if (place == StoragePlace::Embedded) {
			if (initialization) {
				LLVMValueRef ownerSlot = nullptr;
				if (initialization->heapCell) {
					const LLVMTargetDataRef target = LLVMGetModuleDataLayout(mod_module);
					const LLVMTypeRef       word   = LLVMIntPtrTypeInContext(LLVMGetModuleContext(mod_module), target);
					ownerSlot                      = allocateEntrySlot(builder, LLVMTypeOf(address));
					ir_buffer = callRuntime(builder, mod_module, "cloth_rt_cell_backing", LLVMTypeOf(address),
					                        { ownerSlot, LLVMConstInt(word, LLVMABISizeOfType(target, ty_buffer), false),
					                          LLVMConstInt(word, LLVMABIAlignmentOfType(target, ty_buffer), false) });
				} else {
					ir_buffer = allocateBacking(builder, ty_buffer, *initialization);
				}
				if (!sharedShape) sharedShape = array_descriptor::sharedShape(mod_module, at_types.fixedShape);
				LLVMValueRef descriptor = array_descriptor::fixed(builder, at_types.fixedShape, ir_buffer, sharedShape);
				if (ownerSlot)
					descriptor = LLVMBuildInsertValue(builder, descriptor, LLVMBuildLoad2(builder, LLVMTypeOf(address), ownerSlot, ""),
					                                  LLVMCountStructElementTypes(at_types.descriptor) - 1, "");
				LLVMBuildStore(builder, descriptor, address);
			} else {
				ir_buffer = array_descriptor::dataAddress(builder, address);
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
		const LLVMValueRef remembered = LLVMBuildExtractValue(builder, value, u_witness, "");
		const auto        &field      = sl_layout.fields[u_field].type;
		if (growable_array::ownsBuffer(field))
			LLVMBuildStore(builder, remembered, ir_address);
		else
			storeFixedWitness(builder, field, ir_address, LLVMBuildExtractValue(builder, value, u_field, ""), remembered);
	}
}
}
