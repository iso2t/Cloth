// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <backend/class_objects.hpp>
#include <backend/dispatch.hpp>
#include <backend/enum_payloads.hpp>
#include <backend/value_types.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>

namespace backend::detail {

using ResultView = std::span<const cloth::ResolvedType>;

struct FunctionBodyToEmit {
	const cloth::Symbol       *symbol;
	const cloth::GenericInstance *instance;
	LLVMValueRef               function;
	std::optional<std::size_t> defaultParameter;
	const cloth::ResolvedType    *host = nullptr;
};

struct ModuleFunctions {
	ModuleFunctions (const cloth::CheckedProgram &checked, LLVMModuleRef output);

	const cloth::CheckedProgram    &program;
	LLVMModuleRef                   module;
	ValueTypes                      valueTypes{ module, program };
	EnumPayloads                    enumPayloads{ module, program, valueTypes };
	ClassObjects                    objects{ module, program, valueTypes };
	NativeDispatch                  dispatch{ module };
	std::vector<FunctionBodyToEmit> pending = {};

	const cloth::Symbol          *field (const cloth::Token *declaration) const;
	LLVMTypeRef  resultType (ResultView results);
	LLVMTypeRef                   functionType (const cloth::Symbol &symbol, const cloth::GenericInstance *instance, std::optional<std::size_t> index = {});
	const cloth::ResultTypes     &arguments (const cloth::GenericInstance *instance) const;
	LLVMValueRef                  declareMember (const cloth::MethodRef &body, const cloth::ResultTypes &arguments = {});
	const cloth::DeclarationInfo &signature (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) const;
	const cloth::BodyFacts       &facts (const cloth::GenericInstance *instance) const;
	const cloth::BodyFacts       &bodyFacts (const FunctionBodyToEmit &body, const cloth::ResolvedType &receiver) const;
	cloth::ResolvedType           receiverType (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) const;
	LLVMValueRef                  declareFunction (const cloth::Symbol &symbol, const cloth::GenericInstance *instance = nullptr);
	LLVMValueRef                  declareDefault (const cloth::Symbol &contract, std::size_t index, const cloth::GenericInstance *instance);
	void                          declareExports ();
	void                          recordInterfaces ();
	void                          emitNativeEntry (LLVMBuilderRef builder, LLVMValueRef function);

    private:
	LLVMValueRef declare (const cloth::Symbol &symbol, const cloth::GenericInstance *instance, std::optional<std::size_t> index,
	                      const cloth::ResolvedType *host = nullptr);

	std::unordered_map<const cloth::GenericInstance *, const cloth::InstanceKey *> m_instanceKeys;
	std::unordered_map<const cloth::Token *, const cloth::Symbol *>                m_fields;
	using FunctionKey = std::tuple<const cloth::Symbol *, const cloth::InstanceKey *, std::optional<std::size_t>, const cloth::ResolvedType *>;
	std::map<FunctionKey, LLVMValueRef> m_functions;
};

LLVMTypeRef  structReceiverType (LLVMContextRef context);
LLVMValueRef packResults (LLVMBuilderRef builder, std::span<LLVMValueRef const> values);
bool         hasObjectReceiver (const cloth::Symbol &symbol, bool isDefault);
bool         isStoredField (const cloth::Symbol *field);
LLVMValueRef selectResult (LLVMBuilderRef builder, LLVMValueRef value, std::size_t count, std::size_t index);
}
