// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <map>

#include <llvm-c/Core.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

class ValueTypes {
    public:
	ValueTypes (LLVMModuleRef module, const cloth::CheckedProgram &program);
	[[nodiscard]] LLVMTypeRef directCallType (const cloth::ResolvedType &type) const;
	// Returns null when the checked type has no supported LLVM value representation.
	[[nodiscard]] LLVMTypeRef get (const cloth::ResolvedType &type);

    private:
	LLVMModuleRef                                      mod_module;
	const cloth::CheckedProgram                       &cp_program;
	std::map<const cloth::StructLayout *, LLVMTypeRef> m_structs;
};
}
