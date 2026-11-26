// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/function_abi.hpp>
#include <backend/growable_array.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>
#include <backend/string_value.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
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
const cloth::FunctionDeclaration *exportedFunction (const cloth::Symbol &symbol) {
	if (symbol.kind != cloth::SymbolKind::Function || symbol.visibility != cloth::Visibility::Public) return nullptr;
	const auto *fn = dynamic_cast<const cloth::FunctionDeclaration *>(std::get<const cloth::CallableSignature *>(symbol.declaration));
	return fn && !fn->typeParameters ? fn : nullptr;
}

bool exportType (const cloth::ResolvedType &type) {
	const cloth::ResolvedType leaf = type.nullable() ? type.element() : type;
	return !leaf.declaration && leaf.suffixes.empty() &&
	       (leaf.integer() || leaf.floating() || leaf == cloth::ResolvedType(cloth::KBoolean) || leaf == cloth::ResolvedType(cloth::KCharacter) ||
	        leaf == cloth::ResolvedType(cloth::KString));
}

// Hex-encoded UTF-8 components cannot contain the separator. Paths and declaration order never enter the link name.
void appendComponent (std::string &name, std::string_view part) {
	static constexpr char digits[] = "0123456789abcdef";
	name += '_';
	for (const unsigned char byte : part) {
		name += digits[byte >> 4u];
		name += digits[byte & 15u];
	}
}

void appendExportType (std::string &name, const cloth::ResolvedType &type) {
	appendComponent(name, type.name);
	name += "_w" + std::to_string(type.width);
	if (type.nullable()) name += "_q";
}

std::optional<std::string> exportName (const std::string &module, const cloth::Symbol &symbol, const cloth::DeclarationInfo &signature) {
	if (!std::ranges::all_of(signature.parameters, exportType) || !std::ranges::all_of(signature.returns, exportType)) return std::nullopt;
	std::string s_name = "cloth_v1";
	appendComponent(s_name, module);
	appendComponent(s_name, symbol.name.getNameKey());
	for (const cloth::ResolvedType &type : signature.parameters) appendExportType(s_name, type);
	s_name += "_r";
	for (const auto &type : signature.returns) appendExportType(s_name, type);
	return s_name;
}

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
	if (body.host && !body.instance) {
		const auto *owner = program.declarations.at(body.symbol).owner;
		if (owner && owner->kind == cloth::SymbolKind::State && !program.declarations.at(owner).owner) return program.attachmentBodies.at(*body.host);
	}
	const auto *callable = std::get_if<const cloth::CallableSignature *>(&body.symbol->declaration);
	if (!body.defaultParameter && callable && dynamic_cast<const cloth::PrimaryConstructorDeclaration *>(*callable))
		return facts(program.typeInstance(receiver));
	return facts(body.instance);
}

cloth::ResolvedType ModuleFunctions::receiverType (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) const {
	if (!hasObjectReceiver(symbol, false)) return cloth::ResolvedType{};
	if (instance) {
		const auto &receiver = m_instanceKeys.at(instance)->receiver;
		if (receiver) return *receiver;
	}
	const cloth::Symbol *owner = program.declarations.at(&symbol).owner;
	if (owner && owner->kind == cloth::SymbolKind::State) owner = program.declarations.at(owner).owner;
	if (!owner) return cloth::ResolvedType{};
	cloth::ResolvedType type(owner->name.getNameKey());
	type.declaration = owner;
	return type;
}

LLVMValueRef ModuleFunctions::declareFunction (const cloth::Symbol &symbol, const cloth::GenericInstance *instance) {
	return declare(symbol, instance, std::nullopt);
}

bool hasObjectReceiver (const cloth::Symbol &symbol, bool isDefault) {
	return symbol.kind == cloth::SymbolKind::StateMethodContract || symbol.kind == cloth::SymbolKind::StateInitializer ||
	       symbol.kind == cloth::SymbolKind::State || symbol.kind == cloth::SymbolKind::StateAttachment || symbol.kind == cloth::SymbolKind::Method ||
	       symbol.kind == cloth::SymbolKind::Operator || symbol.kind == cloth::SymbolKind::Destructor ||
	       (symbol.kind == cloth::SymbolKind::Constructor && !isDefault);
}

LLVMValueRef ModuleFunctions::declareDefault (const cloth::Symbol &contract, std::size_t index, const cloth::GenericInstance *instance) {
	return declare(contract, instance, index);
}

const cloth::ResultTypes &ModuleFunctions::arguments (const cloth::GenericInstance *instance) const {
	static const cloth::ResultTypes empty;
	return instance ? m_instanceKeys.at(instance)->arguments : empty;
}

LLVMValueRef ModuleFunctions::declareMember (const cloth::MethodRef &body, const cloth::ResultTypes &arguments) {
	const auto  found     = program.instances.find({ .declaration = body.symbol, .receiver = body.owner, .arguments = arguments });
	const auto *owner     = program.declarations.at(body.symbol).owner;
	const bool  stateBody = body.symbol->kind == cloth::SymbolKind::State || body.symbol->kind == cloth::SymbolKind::StateAttachment ||
	                       (owner && owner->kind == cloth::SymbolKind::State);
	const cloth::ResolvedType *host = stateBody ? &program.dispatchTables.find(body.owner)->first : nullptr;
	if (found == program.instances.end() && (!arguments.empty() || !body.owner.arguments().empty()))
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedFunctionAbi, "Missing checked instance for a dispatched body",
		                           { { .span = body.symbol->name.getSpan(), .text = "missing method specialization" } });
	return declare(*body.symbol, found == program.instances.end() ? nullptr : &found->second, {}, host);
}

LLVMTypeRef ModuleFunctions::functionType (const cloth::Symbol &symbol, const cloth::GenericInstance *instance, std::optional<std::size_t> index) {
	const cloth::DeclarationInfo &signature = this->signature(symbol, instance);
	const ResultView              parameters(signature.parameters);
	const ResultView              results = index ? parameters.subspan(*index, 1) : ResultView(signature.returns);
	std::vector<LLVMTypeRef>      v_parameters;
	if (hasObjectReceiver(symbol, index.has_value())) {
		const LLVMContextRef context = LLVMGetModuleContext(module);
		const auto           receiver = receiverType(symbol, instance);
		v_parameters.push_back(program.structLayouts.contains(receiver)             ? structReceiverType(context) :
		                       receiver.enumeration() || receiver.primitiveFamily() ? valueTypes.localType(receiver) :
		                                                                              LLVMInt64TypeInContext(context));
	}
	for (const cloth::ResolvedType &type : parameters.first(index.value_or(parameters.size()))) v_parameters.push_back(valueTypes.localType(type));
	const LLVMTypeRef ty_result = resultType(results);
	if (!ty_result || std::ranges::find(v_parameters, nullptr) != v_parameters.end())
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedFunctionAbi, "LLVM lowering requires representable parameter and result types",
		                           { { .span = symbol.name.getSpan(), .text = "unsupported function signature" } });
	return LLVMFunctionType(ty_result, v_parameters.data(), static_cast<unsigned>(v_parameters.size()), false);
}

LLVMValueRef ModuleFunctions::declare (const cloth::Symbol &symbol, const cloth::GenericInstance *instance, std::optional<std::size_t> index,
                                       const cloth::ResolvedType *host) {
	const FunctionKey key{ &symbol, instance ? m_instanceKeys.at(instance) : nullptr, index, host };
	if (const auto found = m_functions.find(key); found != m_functions.end()) return found->second;
	const bool  b_native  = !index && &symbol == program.entryPoint && nativeSignature(signature(symbol, instance));
	std::string s_name    = b_native ? "main" : "cloth." + symbol.name.getNameKey();
	const auto &info      = program.declarations.at(&symbol);
	const auto  it_module = program.nativeModules.find(info.file);
	const auto *fn_export = !instance && !info.owner && it_module != program.nativeModules.end() ? exportedFunction(symbol) : nullptr;
	if (fn_export) {
		auto name = exportName(it_module->second, symbol, info);
		if (!name)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedFunctionAbi,
			                           "A native library export requires scalar or string parameters and results",
			                           { { .span = symbol.name.getSpan(), .text = "unsupported native export signature" } });
		s_name = std::move(*name);
	}
	if (index) s_name += ".default." + std::to_string(*index);
	const LLVMTypeRef  ty_function = functionType(symbol, instance, index);
	const LLVMValueRef fn_function = LLVMAddFunction(module, s_name.c_str(), ty_function);
	LLVMSetLinkage(fn_function, b_native || fn_export ? LLVMExternalLinkage : LLVMInternalLinkage);
	m_functions.emplace(key, fn_function);
	if (fn_export && info.file != 0) return fn_function;
	pending.push_back({ .symbol = &symbol, .instance = instance, .function = fn_function, .defaultParameter = index, .host = host });
	return fn_function;
}

void ModuleFunctions::declareExports () {
	for (const auto &[name, symbols] : program.units.front()->getGlobals()) {
		for (const auto &symbol : symbols) {
			const auto *fn = exportedFunction(symbol);
			if (!fn) continue;
			declareFunction(symbol);
			for (std::size_t i = 0; i < fn->parameters.size(); ++i)
				if (fn->parameters[i].defaultValue) declareDefault(symbol, i, nullptr);
		}
	}
}

void ModuleFunctions::recordInterfaces () {
	// Named calls and omitted arguments depend on information absent from the raw link name.
	std::vector<std::string> modules;
	for (const auto &[file, name] : program.nativeModules) {
		std::vector<std::string> exports;
		for (const auto &[key, symbols] : program.units[file]->getGlobals()) {
			for (const auto &symbol : symbols) {
				const auto *fn = exportedFunction(symbol);
				if (!fn) continue;
				// An unused import may have an unsupported export. Such an API cannot match an emitted provider.
				std::string signature = exportName(name, symbol, program.declarations.at(&symbol)).value_or("unsupported");
				for (const auto &parameter : fn->parameters) {
					appendComponent(signature, parameter.name.getNameKey());
					if (parameter.defaultValue) signature += '?';
				}
				exports.push_back(std::move(signature));
			}
		}
		std::ranges::sort(exports);
		std::string line = name + '\t';
		for (const auto &signature : exports) {
			line += signature;
			line += ';';
		}
		modules.push_back(std::move(line));
	}
	std::ranges::sort(modules);
	// Shuttle reads three header lines followed by one tab-separated module name and export list per line.
	const auto  producer = program.nativeModules.find(0);
	std::string data     = "cloth-interface-v1\n";
	data += LLVMGetTarget(module);
	data += '\n';
	if (producer != program.nativeModules.end()) data += producer->second;
	data += '\n';
	for (const auto &line : modules) {
		data += line;
		data += '\n';
	}
	const auto contents = string_value::literalBytes(LLVMGetModuleContext(module), data);
	const auto global   = LLVMAddGlobal(module, LLVMTypeOf(contents), "cloth.interfaces");
	LLVMSetInitializer(global, contents);
	LLVMSetGlobalConstant(global, true);
	LLVMSetLinkage(global, LLVMPrivateLinkage);
	LLVMSetAlignment(global, 1);
	const bool machO = std::string_view(LLVMGetDataLayoutStr(module)).find("m:o") != std::string_view::npos;
	LLVMSetSection(global, machO ? "__DATA,__clapi" : ".clapi");
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
