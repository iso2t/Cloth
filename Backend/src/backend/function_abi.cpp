// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/function_abi.hpp>
#include <backend/growable_array.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace backend::detail {
namespace {
bool nativeSignature (const cloth::DeclarationInfo &signature) {
	return signature.parameters.empty() && signature.returns.size() == 1 && signature.returns.front() == cloth::ResolvedType(cloth::KInteger);
}
}

ModuleFunctions::ModuleFunctions (const cloth::CheckedProgram &checked, LLVMModuleRef output) : program(checked), module(output) {
	for (const auto &[key, instance] : program.instances) m_instanceKeys.emplace(&instance, &key);
	for (const auto &[symbol, info] : program.declarations)
		if (isStoredField(symbol)) m_fields.emplace(&symbol->name, symbol);
}

bool isStoredField (const cloth::Symbol *field) {
	return field && (field->classFieldIndex || std::holds_alternative<const cloth::StructFieldDeclaration *>(field->declaration));
}

const cloth::Symbol *ModuleFunctions::field (const cloth::Token *declaration) const {
	const auto found = m_fields.find(declaration);
	return found == m_fields.end() ? nullptr : found->second;
}

LLVMTypeRef ModuleFunctions::resultType (ResultView results) {
	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	if (results.empty()) return LLVMVoidTypeInContext(ctx_context);
	if (results.size() == 1) return valueTypes.localType(results.front());
	std::vector<LLVMTypeRef> v_fields;
	v_fields.reserve(results.size());
	for (const cloth::ResolvedType &type : results) {
		const LLVMTypeRef ty_field = valueTypes.localType(type);
		if (!ty_field) return nullptr;
		v_fields.push_back(ty_field);
	}
	return LLVMStructTypeInContext(ctx_context, v_fields.data(), static_cast<unsigned>(v_fields.size()), false);
}

LLVMTypeRef structReceiverType (LLVMContextRef context) {
	LLVMTypeRef fields[] = { LLVMPointerTypeInContext(context, 0), LLVMInt64TypeInContext(context), LLVMInt8TypeInContext(context) };
	return LLVMStructTypeInContext(context, fields, 3, false);
}

LLVMValueRef packResults (LLVMBuilderRef builder, std::span<LLVMValueRef const> values) {
	if (values.empty()) return nullptr;
	if (values.size() == 1) return values.front();
	std::vector<LLVMTypeRef> v_types;
	v_types.reserve(values.size());
	for (const LLVMValueRef ir_value : values) v_types.push_back(LLVMTypeOf(ir_value));
	LLVMValueRef ir_result =
	        LLVMGetUndef(LLVMStructTypeInContext(LLVMGetTypeContext(v_types.front()), v_types.data(), static_cast<unsigned>(v_types.size()), false));
	for (unsigned u_index = 0; u_index < values.size(); ++u_index) ir_result = LLVMBuildInsertValue(builder, ir_result, values[u_index], u_index, "");
	return ir_result;
}

LLVMValueRef selectResult (LLVMBuilderRef builder, LLVMValueRef value, std::size_t count, std::size_t index) {
	return count == 1 ? value : LLVMBuildExtractValue(builder, value, static_cast<unsigned>(index), "");
}

const cloth::DeclarationInfo &ModuleFunctions::signature (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) const {
	if (!instance) return program.declarations.at(&symbol);
	if (m_instanceKeys.at(instance)->declaration == &symbol) return instance->signature;
	return instance->constructorContracts.at(&symbol);
}

const cloth::BodyFacts &ModuleFunctions::facts (const cloth::GenericInstance *instance) const {
	return instance ? instance->body : static_cast<const cloth::BodyFacts &>(program);
}

const cloth::BodyFacts &ModuleFunctions::bodyFacts (const FunctionBodyToEmit &body, const cloth::ResolvedType &receiver) const {
	// Parent calls and field initializers are checked on the enclosing type. Parameter defaults belong to the callable.
	const auto *callable = std::get<const cloth::CallableSignature *>(body.symbol->declaration);
	if (!body.defaultParameter && dynamic_cast<const cloth::PrimaryConstructorDeclaration *>(callable)) return facts(program.typeInstance(receiver));
	return facts(body.instance);
}

cloth::ResolvedType ModuleFunctions::receiverType (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) const {
	if (!hasObjectReceiver(symbol, false)) return cloth::ResolvedType{};
	if (instance) {
		const auto &receiver = m_instanceKeys.at(instance)->receiver;
		if (receiver) return *receiver;
	}
	const cloth::Symbol *owner = program.declarations.at(&symbol).owner;
	if (!owner) return cloth::ResolvedType{};
	cloth::ResolvedType type(owner->name.getNameKey());
	type.declaration = owner;
	return type;
}

LLVMValueRef ModuleFunctions::declareFunction (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) {
	return declare(symbol, instance, std::nullopt);
}

bool hasObjectReceiver (const cloth::Symbol &symbol, bool isDefault) {
	return symbol.kind == cloth::SymbolKind::Method || symbol.kind == cloth::SymbolKind::Operator || symbol.kind == cloth::SymbolKind::Destructor ||
	       (symbol.kind == cloth::SymbolKind::Constructor && !isDefault);
}

LLVMValueRef ModuleFunctions::declareDefault (const cloth::Symbol &contract, std::size_t index, const cloth::GenericInstance *instance) {
	return declare(contract, instance, index);
}

LLVMValueRef ModuleFunctions::declare (const cloth::Symbol &symbol, const cloth::GenericInstance *instance, std::optional<std::size_t> index) {
	const FunctionKey key{ &symbol, instance ? m_instanceKeys.at(instance) : nullptr, index };
	if (const auto found = m_functions.find(key); found != m_functions.end()) return found->second;
	const cloth::DeclarationInfo &signature = this->signature(symbol, instance);
	const ResultView              parameters(signature.parameters);
	const ResultView              results = index ? parameters.subspan(*index, 1) : ResultView(signature.returns);
	std::vector<LLVMTypeRef>      v_parameters;
	if (hasObjectReceiver(symbol, index.has_value())) {
		const LLVMContextRef context = LLVMGetModuleContext(module);
		v_parameters.push_back(program.structLayouts.contains(receiverType(symbol, instance)) ? structReceiverType(context) :
		                                                                                        LLVMInt64TypeInContext(context));
	}
	for (const cloth::ResolvedType &type : parameters.first(index.value_or(parameters.size()))) v_parameters.push_back(valueTypes.localType(type));
	const LLVMTypeRef ty_result = resultType(results);
	if (!ty_result || std::ranges::find(v_parameters, nullptr) != v_parameters.end())
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedFunctionAbi, "LLVM lowering requires representable parameter and result types",
		                           { { .span = symbol.name.getSpan(), .text = "unsupported function signature" } });
	const bool  b_native = !index && &symbol == program.entryPoint && nativeSignature(signature);
	std::string s_name   = b_native ? "main" : "cloth." + symbol.name.getNameKey();
	if (index) s_name += ".default." + std::to_string(*index);
	const LLVMTypeRef  ty_function = LLVMFunctionType(ty_result, v_parameters.data(), static_cast<unsigned>(v_parameters.size()), false);
	const LLVMValueRef fn_function = LLVMAddFunction(module, s_name.c_str(), ty_function);
	LLVMSetLinkage(fn_function, b_native ? LLVMExternalLinkage : LLVMInternalLinkage);
	m_functions.emplace(key, fn_function);
	pending.push_back({ .symbol = &symbol, .instance = instance, .function = fn_function, .defaultParameter = index });
	return fn_function;
}

void ModuleFunctions::emitNativeEntry (LLVMBuilderRef builder, LLVMValueRef function) {
	const cloth::DeclarationInfo &signature = program.declarations.at(program.entryPoint);
	if (nativeSignature(signature)) return;
	const LLVMContextRef ctx_context    = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_status      = LLVMInt32TypeInContext(ctx_context);
	const LLVMTypeRef    ty_pointer     = LLVMPointerTypeInContext(ctx_context, 0);
	LLVMTypeRef          v_parameters[] = { ty_status, ty_pointer };
	const bool           b_arguments    = !signature.parameters.empty();
	const LLVMValueRef   fn_main        = LLVMAddFunction(module, "main", LLVMFunctionType(ty_status, v_parameters, b_arguments ? 2 : 0, false));
	LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(ctx_context, fn_main, "entry"));
	LLVMValueRef ir_arguments = nullptr, ir_cells = nullptr, ir_count = nullptr;
	if (b_arguments) {
		const cloth::ResolvedType &type         = signature.parameters.front();
		const LLVMTypeRef          ty_array     = valueTypes.localType(type.nullable() ? type.element() : type);
		const LLVMTypeRef          ty_word      = LLVMIntPtrTypeInContext(ctx_context, LLVMGetModuleDataLayout(module));
		const LLVMValueRef         ir_length    = LLVMBuildAlloca(builder, ty_word, "");
		LLVMValueRef               v_inputs[]   = { LLVMGetParam(fn_main, 0), LLVMGetParam(fn_main, 1), ir_length };
		const LLVMValueRef         fn_arguments = declareProcessArguments(module);
		ir_cells                                = LLVMBuildCall2(builder, LLVMGlobalGetValueType(fn_arguments), fn_arguments, v_inputs, 3, "");
		ir_count                                = LLVMBuildLoad2(builder, ty_word, ir_length, "");
		ir_arguments                            = growable_array::borrow(builder, ty_array, ir_cells, ir_count);
		if (type.nullable()) {
			const LLVMValueRef ir_present =
			        LLVMBuildZExt(builder, LLVMBuildIsNotNull(builder, ir_count, ""), LLVMInt8TypeInContext(ctx_context), "");
			ir_arguments = nullable::pack(builder, ir_arguments, ir_present);
		}
	}
	const LLVMValueRef ir_result = LLVMBuildCall2(builder, LLVMGlobalGetValueType(function), function, &ir_arguments, b_arguments ? 1 : 0, "");
	if (b_arguments) {
		const LLVMValueRef fn_release = declareReleaseArguments(module);
		LLVMValueRef       v_inputs[] = { ir_cells, ir_count };
		LLVMBuildCall2(builder, LLVMGlobalGetValueType(fn_release), fn_release, v_inputs, 2, "");
	}
	LLVMBuildRet(builder, signature.returns.empty() ? LLVMConstNull(ty_status) : LLVMBuildIntCast2(builder, ir_result, ty_status, true, ""));
}
}
