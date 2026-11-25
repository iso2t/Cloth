// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <backend/value_types.hpp>

#include <initializer_list>
#include <map>
#include <vector>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {
struct ModuleFunctions;

enum class ObjectOperation {
	Create,
	Data,
	CheckBorrow,
	Backing,
	Delete,
	Is,
	Acquire,
	Release,
	CheckMutation,
	SetCleanup,
	Configure,
	Method,
	BeginInitial,
	State,
	Enter,
	Payload
};
LLVMValueRef objectCall (LLVMBuilderRef builder, LLVMModuleRef module, ObjectOperation operation, std::initializer_list<LLVMValueRef> arguments);

struct ClassStorage {
	cloth::ResolvedType                type;
	const cloth::GenericInstance      *instance;
	LLVMTypeRef                        storage;
	const ClassStorage                *base;
	const cloth::Symbol               *destructor;
	std::vector<const cloth::Symbol *> fields;
	cloth::ResultTypes                 nominalViews = {};
	LLVMValueRef                       views = nullptr, destroy = nullptr, dispose = nullptr;
};

class ClassObjects {
    public:
	ClassObjects (LLVMModuleRef module, const cloth::CheckedProgram &program, ValueTypes &types);
	ClassStorage              &layout (const cloth::ResolvedType &type);
	const ClassStorage        &declaringLayout (const cloth::ResolvedType &receiver, const cloth::Symbol &field);
	const cloth::ResolvedType &fieldType (const ClassStorage &owner, const cloth::Symbol &field) const;
	LLVMValueRef               fieldAddress (LLVMBuilderRef builder, LLVMValueRef storage, const ClassStorage &owner, const cloth::Symbol &field);
	LLVMValueRef               typeTag (const cloth::ResolvedType &type);
	LLVMValueRef               create (LLVMBuilderRef builder, const cloth::ResolvedType &type, ModuleFunctions &functions);

    private:
	void                                          describe (const cloth::ResolvedType &type, ClassStorage &storage, ModuleFunctions &functions);
	LLVMModuleRef                                 mod_module;
	const cloth::CheckedProgram                  &cp_program;
	ValueTypes                                   &vt_types;
	std::map<cloth::ResolvedType, ClassStorage>   m_classes;
	std::map<cloth::ResolvedType, LLVMValueRef>   m_tags;
};
}
