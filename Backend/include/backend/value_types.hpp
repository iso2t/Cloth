// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
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

// Fixed arrays have inline cells in a value, a cell buffer in a slot, and a descriptor in a struct field.
enum class StoragePlace { Slot, Embedded, Value };

[[nodiscard]] StoragePlace arrayElementPlace (const cloth::ResolvedType &element);

class ValueTypes {
	struct ArrayPath {
		std::vector<ArrayDimension> dimensions;
		LLVMValueRef                owner    = nullptr;
		LLVMBasicBlockRef           origin   = nullptr;
		bool                        heapCell = false;
	};
	enum class Transfer { Load, Store };
	enum class CellCleanup { None, Dispose, Transfer };

    public:
	ValueTypes (LLVMModuleRef module, const cloth::CheckedProgram &program);
	[[nodiscard]] std::optional<cloth::StorageLayout> aggregateLayout (const std::vector<LLVMTypeRef> &fields) const;
	// Physical storage inside a struct, including fixed-array descriptors.
	[[nodiscard]] LLVMTypeRef get (const cloth::ResolvedType &type);
	// Fixed array buffers store their cells contiguously at the checked stride.
	[[nodiscard]] LLVMTypeRef slotType (const cloth::ResolvedType &type);
	// Fixed array values carry cells inline. Growable array values carry a descriptor.
	[[nodiscard]] LLVMTypeRef localType (const cloth::ResolvedType &type);
	[[nodiscard]] bool        ownsBuffers (const cloth::ResolvedType &type) const;
	LLVMValueRef              metadata (const cloth::ResolvedType &type);
	LLVMTypeRef               metadataType () const;
	LLVMValueRef              bufferDisposer (const cloth::ResolvedType &type, StoragePlace place = StoragePlace::Value);
	cloth::StorageLayout      cellLayout (const cloth::ResolvedType &element);
	LLVMValueRef              cellOperations (const cloth::ResolvedType &element);
	LLVMValueRef              prepareCell (LLVMBuilderRef builder, const cloth::ResolvedType &element, LLVMValueRef value);
	LLVMValueRef              takeCell (LLVMBuilderRef builder, const cloth::ResolvedType &element, LLVMValueRef slot);
	LLVMValueRef insertStructField (LLVMBuilderRef builder, const cloth::ResolvedType &owner, LLVMValueRef aggregate, unsigned index, LLVMValueRef value);
	LLVMValueRef arrayFieldWitness (LLVMBuilderRef builder, const cloth::ResolvedType &owner, LLVMValueRef parentAddress, unsigned fieldIndex);
	void         updateGrowableWitness (LLVMBuilderRef builder, const cloth::ResolvedType &fieldType, LLVMValueRef address, LLVMValueRef witness);
	LLVMValueRef              arrayData (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot);
	LLVMValueRef              allocateLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef value);
	void initializeSlot (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value, LLVMValueRef self = nullptr);
	LLVMValueRef loadLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, StoragePlace place = StoragePlace::Slot);
	void         storeLocal (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value,
	                         StoragePlace place = StoragePlace::Slot, LLVMValueRef witness = nullptr);

    private:
	std::map<cloth::ResolvedType, LLVMValueRef> m_metadata;
	bool                                        flatCell (const cloth::ResolvedType &type) const;
	LLVMTypeRef                                 storageType (const cloth::ResolvedType &type, StoragePlace place);
	LLVMValueRef                                bufferOperation (const cloth::ResolvedType &type, bool copy, StoragePlace place);
	void         visitBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef target, LLVMValueRef source, StoragePlace place,
	                           CellCleanup cleanup = CellCleanup::None);
	void         visitFixedCells (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef target, LLVMValueRef source, StoragePlace place,
	                              CellCleanup cleanup);
	void         initializeCell (LLVMBuilderRef builder, const cloth::ResolvedType &element, LLVMValueRef slot, LLVMValueRef value);
	LLVMValueRef cloneBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef value);
	std::map<cloth::ResolvedType, LLVMValueRef> m_cellOperations;
	LLVMValueRef replaceBuffers (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef slot, LLVMValueRef value, StoragePlace place);
	std::map<std::tuple<cloth::ResolvedType, bool, StoragePlace>, LLVMValueRef> m_bufferOperations;
	const StructTypes                                                          *structTypes (const cloth::ResolvedType &type);
	const ArrayTypes  *arrayTypes (const cloth::ResolvedType &type);
	LLVMTypeRef        nullableType (LLVMTypeRef payload) const;
	std::uint64_t      backingBytes (const cloth::ResolvedType &field);
	LLVMValueRef       allocateBacking (LLVMBuilderRef builder, LLVMTypeRef bufferType, const ArrayPath &path) const;
	void               copyBuffer (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef buffer, LLVMValueRef snapshot, Transfer direction,
	                               ArrayPath *initialization, LLVMValueRef sharedShape = nullptr);
	void               storeValue (LLVMBuilderRef builder, const cloth::ResolvedType &type, LLVMValueRef address, LLVMValueRef value, StoragePlace place,
	                               ArrayPath *initialization, LLVMValueRef sharedShape = nullptr);
	void               storeFixedWitness (LLVMBuilderRef builder, const cloth::ResolvedType &fieldType, LLVMValueRef address, LLVMValueRef value,
	                                      LLVMValueRef remembered);

	LLVMModuleRef                                      mod_module;
	const cloth::CheckedProgram                       &cp_program;
	std::map<const cloth::StructLayout *, StructTypes> m_structs;
	std::map<cloth::ResolvedType, ArrayTypes>          m_arrays;
};
}
