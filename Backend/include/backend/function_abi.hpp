// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <backend/class_objects.hpp>
#include <backend/enum_payloads.hpp>
#include <backend/value_types.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

using ResultView = std::span<const cloth::ResolvedType>;

struct FunctionBodyToEmit {
	const cloth::Symbol       *symbol;
	LLVMValueRef               function;
	std::optional<std::size_t> defaultParameter;
};

struct ModuleFunctions {
	ModuleFunctions (const cloth::CheckedProgram &checked, LLVMModuleRef output);

	const cloth::CheckedProgram    &program;
	LLVMModuleRef                   module;
	ValueTypes                      valueTypes{ module, program };
	EnumPayloads                    enumPayloads{ module, program, valueTypes };
	ClassObjects                    objects{ module, program, valueTypes };
	std::vector<FunctionBodyToEmit> pending = {};

	LLVMTypeRef  resultType (ResultView results);
	LLVMValueRef declareFunction (const cloth::Symbol &symbol);
	LLVMValueRef declareDefault (const cloth::Symbol &contract, std::size_t index);
	void         emitNativeEntry (LLVMBuilderRef builder, LLVMValueRef function);

    private:
	LLVMValueRef declare (const cloth::Symbol &symbol, std::optional<std::size_t> index);

	std::unordered_map<const cloth::Symbol *, LLVMValueRef>               m_functions;
	std::unordered_map<const cloth::ParameterDeclaration *, LLVMValueRef> m_defaults;
};

LLVMValueRef packResults (LLVMBuilderRef builder, std::span<LLVMValueRef const> values);
bool         hasObjectReceiver (const cloth::Symbol &symbol, bool isDefault);
LLVMValueRef selectResult (LLVMBuilderRef builder, LLVMValueRef value, std::size_t count, std::size_t index);
}
