// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <llvm-c/Core.h>
#include <semantic/checked_program.hpp>
#include <semantic/target_layout.hpp>

namespace backend::detail {

// An optional initializer must be available at function entry, where the slot is allocated.
LLVMValueRef allocateEntrySlot (LLVMBuilderRef builder, LLVMTypeRef type, LLVMValueRef initial = nullptr);

struct StructTypes {
	LLVMTypeRef                  storage;
	LLVMTypeRef                  value;
	std::map<unsigned, unsigned> witnesses;
	std::uint64_t                backingBytes;
};

struct ArrayTypes {
	LLVMTypeRef   buffer;
	LLVMTypeRef   value;
	LLVMTypeRef   descriptor;
	LLVMValueRef  fixedShape;
	std::uint64_t backingBytes;
};

struct ArrayDimension {
	std::uint64_t count;
	LLVMValueRef  index;
};

enum class StoragePlace { Slot, Embedded };

[[nodiscard]] StoragePlace arrayElementPlace (const cloth::ResolvedType &element);

class ValueTypes {
	struct ArrayPath {
		std::vector<ArrayDimension> dimensions;
		LLVMValueRef                owner  = nullptr;
		LLVMBasicBlockRef           origin = nullptr;
	};
	enum class Transfer { Load, Store };

    public:
	ValueTypes (LLVMModuleRef module, const cloth::CheckedProgram &program);
	[[nodiscard]] std::optional<cloth::StorageLayout> aggregateLayout (const std::vector<LLVMTypeRef> &fields) const;
	// Physical storage inside a struct, including fixed-array descriptors.
	[[nodiscard]] LLVMTypeRef get (const cloth::ResolvedType &type);
	// Fixed array buffers store their cells contiguously at the checked stride.
	[[nodiscard]] LLVMTypeRef slotType (const cloth::ResolvedType &type);
	// Fixed array values carry cells inline. Growable array values carry a descriptor.
	[[nodiscard]] LLVMTypeRef localType (const cloth::ResolvedType &type);
	LLVMValueRef              arrayFieldWitness (LLVMBuilderRef builder, const cloth::ResolvedType &owner, LLVMValueRef parentAddress, unsigned fieldIndex);
	LLVMValueRef              arrayData (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot);
	LLVMValueRef              allocateLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef value);
	void         initializeField (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value, LLVMValueRef self);
	LLVMValueRef loadLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, StoragePlace place = StoragePlace::Slot);
	void         storeLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value,
	                         StoragePlace place = StoragePlace::Slot, LLVMValueRef witness = nullptr);

    private:
	const StructTypes *structTypes (const cloth::ResolvedType &type);
	const ArrayTypes  *arrayTypes (const cloth::ResolvedType &type);
	LLVMTypeRef        nullableType (LLVMTypeRef payload) const;
	std::uint64_t      backingBytes (const cloth::ResolvedType &field);
	LLVMValueRef       allocateBacking (LLVMBuilderRef builder, LLVMTypeRef bufferType, const ArrayPath &path) const;
	void               copyBuffer (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef buffer, LLVMValueRef snapshot, Transfer direction,
	                               ArrayPath *initialization, LLVMValueRef sharedShape = nullptr);
	void               storeValue (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value, StoragePlace place,
	                               ArrayPath *initialization, LLVMValueRef sharedShape = nullptr);
	void storeWitness (LLVMBuilderRef builder, const cloth::ResolvedType &fieldType, LLVMValueRef address, LLVMValueRef value, LLVMValueRef remembered);

	LLVMModuleRef                                      mod_module;
	const cloth::CheckedProgram                       &cp_program;
	std::map<const cloth::StructLayout *, StructTypes> m_structs;
	std::map<cloth::ResolvedType, ArrayTypes>          m_arrays;
};
}
