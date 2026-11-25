// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <map>
#include <optional>
#include <vector>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <token/token.hpp>

namespace backend::detail {
struct ModuleFunctions;

struct DispatchSlot {
	cloth::CallDispatch            call;
	cloth::ResultTypes             arguments;
	std::optional<cloth::Operator> comparison = {};
};

struct ClassDispatchTable {
	LLVMValueRef                                               descriptor;
	std::vector<LLVMValueRef>                                  methods;
	std::map<const cloth::Symbol *, std::vector<LLVMValueRef>> states;
	std::map<cloth::Operator, LLVMValueRef>                    comparisons = {};
};

class NativeDispatch {
    public:
	explicit NativeDispatch (LLVMModuleRef module);

	LLVMValueRef descriptor (const cloth::ResolvedType &type);
	LLVMValueRef method (LLVMBuilderRef builder, const cloth::CallDispatch &call, const cloth::ResultTypes &arguments, LLVMValueRef self, bool directSelf);
	LLVMValueRef compare (LLVMBuilderRef builder, const cloth::ResolvedType &view, cloth::Operator op, LLVMValueRef self, LLVMValueRef peer,
	                      bool directSelf);
	LLVMValueRef stateTag (const cloth::Symbol &selection);
	LLVMValueRef payloadDisposer (ModuleFunctions &functions, const cloth::ResolvedType &type);
	void         resolve (ModuleFunctions &functions);
	void         finish (const cloth::CheckedProgram &program);

    private:
	LLVMValueRef                                      lookup (LLVMBuilderRef builder, const DispatchSlot &request, LLVMValueRef self, bool directSelf);
	LLVMValueRef                                      comparisonBody (ModuleFunctions &functions, const cloth::TypeDispatch &checked, cloth::Operator op);
	LLVMModuleRef                                     mod_module;
	std::vector<DispatchSlot>                         v_slots;
	std::map<cloth::ResolvedType, ClassDispatchTable> m_tables;
	std::map<const cloth::Symbol *, LLVMValueRef>     m_stateTags;
	std::map<cloth::ResolvedType, LLVMValueRef>       m_disposers;
};
}
