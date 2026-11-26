// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <map>
#include <optional>
#include <vector>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <token/token.hpp>

namespace backend::detail {
struct ClassStorage;
struct ModuleFunctions;

enum class ObjectText { None, TypeName, Formatter };

struct DispatchSlot {
	cloth::CallDispatch            call;
	cloth::ResultTypes             arguments;
	std::optional<cloth::Operator> comparison = {};
	ObjectText                     text       = ObjectText::None;
};

struct StateDispatchRow {
	const cloth::Symbol      *selection;
	std::vector<LLVMValueRef> methods = {};
};

struct ClassDispatchTable {
	const ClassStorage                     &owner;
	const cloth::TypeDispatch              &checked;
	std::vector<LLVMValueRef>               methods     = {};
	std::vector<StateDispatchRow>           states      = {};
	std::map<cloth::Operator, LLVMValueRef> comparisons = {};
};

class NativeDispatch {
    public:
	explicit NativeDispatch (LLVMModuleRef module);

	void         declareTable (ClassStorage &owner, const cloth::TypeDispatch &checked);
	LLVMValueRef method (LLVMBuilderRef builder, const cloth::CallDispatch &call, const cloth::ResultTypes &arguments, LLVMValueRef self, bool directSelf);
	LLVMValueRef compare (LLVMBuilderRef builder, const cloth::ResolvedType &view, cloth::Operator op, LLVMValueRef self, LLVMValueRef peer,
	                      bool directSelf);
	LLVMValueRef format (LLVMBuilderRef builder, const cloth::ResolvedType &view, LLVMValueRef self, bool directSelf, bool callsFormatter);
	LLVMValueRef stateTag (const cloth::Symbol &selection);
	LLVMValueRef payloadDisposer (ModuleFunctions &functions, const cloth::ResolvedType &type);
	void         resolve (ModuleFunctions &functions);
	void         finish ();

    private:
	LLVMValueRef                                      lookup (LLVMBuilderRef builder, const DispatchSlot &request, LLVMValueRef self, bool directSelf);
	LLVMValueRef                                      comparisonBody (ModuleFunctions &functions, const cloth::TypeDispatch &checked, cloth::Operator op);
	LLVMModuleRef                                     mod_module;
	std::vector<DispatchSlot>                         v_slots;
	// First use during lowering fixes the emission order of methods and descriptor globals.
	std::vector<ClassDispatchTable>                   v_tables;
	std::map<const cloth::Symbol *, LLVMValueRef>     m_stateTags;
};
}
