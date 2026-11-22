// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <backend/value_types.hpp>

#include <unordered_map>

namespace backend::detail {

class EnumPayloads {
    public:
	EnumPayloads (LLVMModuleRef module, const cloth::CheckedProgram &program, ValueTypes &types);
	LLVMValueRef read (LLVMBuilderRef builder, const cloth::Symbol &field, LLVMValueRef tag, const cloth::TokenSpan &use);

    private:
	LLVMValueRef table (const cloth::Symbol &field, const cloth::TokenSpan &use);
	LLVMValueRef constant (const cloth::Expression &value, const cloth::ResolvedType &destination);

	LLVMModuleRef                                           mod_module;
	const cloth::CheckedProgram                            &cp_program;
	ValueTypes                                             &vt_types;
	std::unordered_map<const cloth::Symbol *, LLVMValueRef> m_tables;
};
}
