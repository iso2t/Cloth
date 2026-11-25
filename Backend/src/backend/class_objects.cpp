// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/class_objects.hpp>
#include <backend/function_abi.hpp>
#include <backend/growable_array.hpp>
#include <backend/nullable_value.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <diagnostics.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace backend::detail {

LLVMValueRef objectCall (LLVMBuilderRef builder, LLVMModuleRef module, ObjectOperation operation, std::initializer_list<LLVMValueRef> arguments) {
	const LLVMContextRef context = LLVMGetModuleContext(module);
	const LLVMTypeRef    handle = LLVMInt64TypeInContext(context), byte = LLVMInt8TypeInContext(context), pointer = LLVMPointerTypeInContext(context, 0);
	const LLVMTypeRef    word = LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(module)), nothing = LLVMVoidTypeInContext(context);
	const auto           call = [&] (const char *name, LLVMTypeRef result, std::initializer_list<LLVMTypeRef> parameters) {
		std::vector<LLVMTypeRef> types(parameters);
		const LLVMTypeRef        signature = LLVMFunctionType(result, types.data(), static_cast<unsigned>(types.size()), false);
		LLVMValueRef             function  = LLVMGetNamedFunction(module, name);
		if (!function) function = LLVMAddFunction(module, name, signature);
		std::vector<LLVMValueRef> inputs(arguments);
		return LLVMBuildCall2(builder, signature, function, inputs.data(), static_cast<unsigned>(inputs.size()), "");
	};
	switch (operation) {
	case ObjectOperation::Create:
		return call("cloth_rt_object_create", handle, { word, word, pointer, pointer, pointer });
	case ObjectOperation::Data:
		return call("cloth_rt_object_data", pointer, { handle, byte });
	case ObjectOperation::Backing:
		return call("cloth_rt_object_backing", pointer, { handle, word, word });
	case ObjectOperation::Delete:
		return call("cloth_rt_object_delete", nothing, { handle, handle });
	case ObjectOperation::Is:
		return call("cloth_rt_object_is", byte, { handle, pointer, byte });
	case ObjectOperation::Acquire:
		return call("cloth_rt_object_acquire", nothing, { handle, handle });
	case ObjectOperation::Release:
		return call("cloth_rt_object_release", nothing, { handle, handle });
	case ObjectOperation::CheckMutation:
		return call("cloth_rt_object_check_mutation", nothing, { handle });
	case ObjectOperation::SetCleanup:
		return call("cloth_rt_object_set_cleanup", nothing, { handle, byte });
	}
	throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Unknown object runtime operation");
}

ClassObjects::ClassObjects (LLVMModuleRef module, const cloth::CheckedProgram &program, ValueTypes &types) :
        mod_module(module),
        cp_program(program),
        vt_types(types) {
	for (const auto &[symbol, info] : program.declarations)
		if (symbol->classFieldIndex) m_fields.emplace(&symbol->name, symbol);
}

const cloth::Symbol *ClassObjects::field (const cloth::Token *declaration) const {
	const auto it_field = m_fields.find(declaration);
	return it_field == m_fields.end() ? nullptr : it_field->second;
}

const cloth::ResolvedType &ClassObjects::fieldType (const cloth::Symbol &field) const {
	const auto &type = cp_program.declarations.at(&field).value;
	if (!type)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class field has no checked value type",
		                           { { .span = field.name.getSpan(), .text = "expected a stored class field" } });
	return *type;
}

ClassStorage &ClassObjects::layout (const cloth::Symbol &owner) {
	if (const auto it_found = m_classes.find(&owner); it_found != m_classes.end()) return it_found->second;
	ClassStorage result{ .storage = nullptr, .base = nullptr, .destructor = nullptr, .fields = {} };
	const auto  &decl = dynamic_cast<const cloth::ClassDeclaration &>(*std::get<const cloth::TypeDeclaration *>(owner.declaration));
	if (decl.typeParameters)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Native class construction requires a non-generic class",
		                           { { .span = owner.name.getSpan(), .text = "generic class" } });
	std::vector<LLVMTypeRef> fields;
	for (const cloth::ResolvedType &parent : cp_program.declarations.at(&owner).parents) {
		if (!parent.classReference() || !cp_program.declarations.at(parent.declaration).primaryConstructor) continue;
		result.base = parent.declaration;
		fields.push_back(layout(*result.base).storage);
	}
	for (const auto &[name, group] : owner.members) {
		for (const cloth::Symbol &member : group) {
			if (member.kind == cloth::SymbolKind::Destructor) result.destructor = &member;
			if (member.classFieldIndex) result.fields.push_back(&member);
		}
	}
	std::ranges::sort(result.fields, {}, &cloth::Symbol::classFieldIndex);
	for (const cloth::Symbol *symbol : result.fields) {
		const cloth::ResolvedType &type   = fieldType(*symbol);
		const LLVMTypeRef          stored = growable_array::ownsBuffer(type) ? vt_types.localType(type) : vt_types.slotType(type);
		if (!stored)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class field has no native storage representation",
			                           { { .span = symbol->name.getSpan(), .text = "unsupported class field type" } });
		fields.push_back(stored);
	}
	if (!vt_types.aggregateLayout(fields))
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class storage exceeds the selected target's addressable size",
		                           { { .span = owner.name.getSpan(), .text = "class instance is too large" } });
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	result.storage               = LLVMStructCreateNamed(context, ("cloth.object." + owner.name.getNameKey()).c_str());
	LLVMStructSetBody(result.storage, fields.data(), static_cast<unsigned>(fields.size()), false);
	return m_classes.emplace(&owner, std::move(result)).first->second;
}

LLVMValueRef ClassObjects::fieldAddress (LLVMBuilderRef builder, LLVMValueRef storage, const cloth::Symbol &field) {
	if (!field.classFieldIndex)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class field has no checked storage index",
		                           { { .span = field.name.getSpan(), .text = "expected a stored class field" } });
	const ClassStorage &owner = layout(*cp_program.declarations.at(&field).owner);
	const auto          index = static_cast<unsigned>(field.classFieldIndex.value() + (owner.base ? 1 : 0));
	return LLVMBuildStructGEP2(builder, owner.storage, storage, index, "");
}

LLVMValueRef ClassObjects::typeTag (const cloth::ResolvedType &type) {
	if (const auto found = m_tags.find(type); found != m_tags.end()) return found->second;
	const LLVMTypeRef  byte = LLVMInt8TypeInContext(LLVMGetModuleContext(mod_module));
	const LLVMValueRef tag  = LLVMAddGlobal(mod_module, byte, ("cloth.type." + type.spelling()).c_str());
	LLVMSetInitializer(tag, LLVMConstNull(byte));
	LLVMSetLinkage(tag, LLVMInternalLinkage);
	LLVMSetGlobalConstant(tag, true);
	return m_tags.emplace(type, tag).first->second;
}

void ClassObjects::describe (const cloth::ResolvedType &type, ClassStorage &storage, ModuleFunctions &functions) {
	const LLVMContextRef      context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef         pointer = LLVMPointerTypeInContext(context, 0);
	const LLVMValueRef        absent  = LLVMConstNull(pointer);
	std::vector<LLVMValueRef> tags;
	for (const cloth::ResolvedType *view = &type; view;) {
		tags.push_back(typeTag(*view));
		const cloth::ResultTypes &parents = cp_program.declarations.at(view->declaration).parents;
		const auto                base    = std::ranges::find_if(parents, &cloth::ResolvedType::classReference);
		view                              = base == parents.end() ? nullptr : &*base;
	}
	if (const auto table = cp_program.dispatchTables.find(type); table != cp_program.dispatchTables.end()) {
		for (const cloth::ResolvedType &view : table->second.interfaces) tags.push_back(typeTag(view));
	}
	tags.push_back(absent);
	const LLVMValueRef list = LLVMConstArray2(pointer, tags.data(), tags.size());
	storage.views           = LLVMAddGlobal(mod_module, LLVMTypeOf(list), "cloth.object.views");
	LLVMSetInitializer(storage.views, list);
	LLVMSetLinkage(storage.views, LLVMInternalLinkage);
	LLVMSetGlobalConstant(storage.views, true);

	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> builder(LLVMCreateBuilderInContext(context), LLVMDisposeBuilder);
	const auto callback = [&] (LLVMValueRef &function, const char *name, LLVMTypeRef parameter) {
		if (!function) {
			function = LLVMAddFunction(mod_module, name, LLVMFunctionType(LLVMVoidTypeInContext(context), &parameter, 1, false));
			LLVMSetLinkage(function, LLVMInternalLinkage);
			LLVMAppendBasicBlockInContext(context, function, "entry");
		}
		LLVMPositionBuilderAtEnd(builder.get(), LLVMGetEntryBasicBlock(function));
		return LLVMGetParam(function, 0);
	};
	for (const ClassStorage *current = &storage; current; current = current->base ? &layout(*current->base) : nullptr) {
		if (current->destructor) {
			LLVMValueRef       self       = callback(storage.destroy, "cloth.destroy", LLVMInt64TypeInContext(context));
			const LLVMValueRef destructor = functions.declareFunction(*current->destructor);
			LLVMBuildCall2(builder.get(), LLVMGlobalGetValueType(destructor), destructor, &self, 1, "");
		}
		for (const cloth::Symbol *field : current->fields) {
			const cloth::ResolvedType &type = fieldType(*field);
			if (!growable_array::ownsBuffer(type)) continue;
			const LLVMValueRef data = callback(storage.dispose, "cloth.dispose", pointer);
			LLVMValueRef       slot = fieldAddress(builder.get(), data, *field);
			if (type.nullable()) slot = nullable::payloadAddress(builder.get(), vt_types.localType(type.element()), slot);
			growable_array::release(builder.get(), mod_module, slot);
		}
	}
	for (const LLVMValueRef function : { storage.destroy, storage.dispose }) {
		if (!function) continue;
		LLVMPositionBuilderAtEnd(builder.get(), LLVMGetEntryBasicBlock(function));
		LLVMBuildRetVoid(builder.get());
	}
}

LLVMValueRef ClassObjects::create (LLVMBuilderRef builder, const cloth::ResolvedType &type, ModuleFunctions &functions) {
	ClassStorage &storage = layout(*type.declaration);
	if (!storage.views) describe(type, storage, functions);
	const LLVMContextRef    context = LLVMGetModuleContext(mod_module);
	const LLVMTargetDataRef target  = LLVMGetModuleDataLayout(mod_module);
	const LLVMTypeRef       word    = LLVMIntPtrTypeInContext(context, target);
	const LLVMValueRef      absent  = LLVMConstNull(LLVMPointerTypeInContext(context, 0));
	return objectCall(builder, mod_module, ObjectOperation::Create,
	                  { LLVMConstInt(word, LLVMABISizeOfType(target, storage.storage), false),
	                    LLVMConstInt(word, LLVMABIAlignmentOfType(target, storage.storage), false), storage.views,
	                    storage.destroy ? storage.destroy : absent, storage.dispose ? storage.dispose : absent });
}
}
