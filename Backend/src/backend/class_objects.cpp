// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/class_objects.hpp>
#include <backend/function_abi.hpp>
#include <backend/growable_array.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

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
	case ObjectOperation::CheckBorrow:
		return call("cloth_rt_object_check_borrow", nothing, { handle, byte });
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
	case ObjectOperation::Configure:
		return call("cloth_rt_object_configure", nothing, { handle, pointer });
	case ObjectOperation::Method:
		return call("cloth_rt_object_method", pointer, { handle, word, byte });
	case ObjectOperation::BeginInitial:
		return call("cloth_rt_object_begin_initial", byte, { handle, pointer });
	case ObjectOperation::State:
		return call("cloth_rt_object_state", pointer, { handle, byte });
	case ObjectOperation::Enter:
		return call("cloth_rt_object_enter", pointer, { handle, pointer, word, word, pointer });
	case ObjectOperation::Payload:
		return call("cloth_rt_object_payload", pointer, { handle, pointer });
	}
	throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Unknown object runtime operation");
}

ClassObjects::ClassObjects (LLVMModuleRef module, const cloth::CheckedProgram &program, ValueTypes &types) :
        mod_module(module),
        cp_program(program),
        vt_types(types) {
}

const cloth::ResolvedType &ClassObjects::fieldType (const ClassStorage &owner, const cloth::Symbol &field) const {
	if (owner.instance) return owner.instance->fields.at(&field);
	const auto &type = cp_program.declarations.at(&field).value;
	if (!type)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class field has no checked value type",
		                           { { .span = field.name.getSpan(), .text = "expected a stored class field" } });
	return *type;
}

ClassStorage &ClassObjects::layout (const cloth::ResolvedType &type) {
	if (const auto it_found = m_classes.find(type); it_found != m_classes.end()) return it_found->second;
	const cloth::Symbol &owner = *type.declaration;
	ClassStorage result{ .type = type, .instance = cp_program.typeInstance(type), .storage = nullptr, .base = nullptr, .destructor = nullptr, .fields = {} };
	std::vector<LLVMTypeRef> fields;
	for (const cloth::ResolvedType &parent : (result.instance ? result.instance->signature : cp_program.declarations.at(&owner)).parents) {
		if (!parent.classReference() || !cp_program.declarations.at(parent.declaration).primaryConstructor) continue;
		result.base = &layout(parent);
		fields.push_back(result.base->storage);
	}
	for (const auto &[name, group] : owner.members) {
		for (const cloth::Symbol &member : group) {
			if (member.kind == cloth::SymbolKind::Destructor) result.destructor = &member;
			if (member.classFieldIndex) result.fields.push_back(&member);
		}
	}
	std::ranges::sort(result.fields, {}, &cloth::Symbol::classFieldIndex);
	for (const cloth::Symbol *symbol : result.fields) {
		const cloth::ResolvedType &type   = fieldType(result, *symbol);
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
	result.storage               = LLVMStructCreateNamed(context, ("cloth.object." + type.spelling()).c_str());
	LLVMStructSetBody(result.storage, fields.data(), static_cast<unsigned>(fields.size()), false);
	return m_classes.emplace(type, std::move(result)).first->second;
}

const ClassStorage &ClassObjects::declaringLayout (const cloth::ResolvedType &receiver, const cloth::Symbol &field) {
	const cloth::Symbol *declaration = cp_program.declarations.at(&field).owner;
	for (const ClassStorage *owner = &layout(receiver); owner; owner = owner->base)
		if (owner->type.declaration == declaration) return *owner;
	throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class field does not belong to the checked receiver",
	                           { { .span = field.name.getSpan(), .text = "invalid field owner" } });
}

LLVMValueRef ClassObjects::fieldAddress (LLVMBuilderRef builder, LLVMValueRef storage, const ClassStorage &owner, const cloth::Symbol &field) {
	if (!field.classFieldIndex)
		throw cde::ClothDiagnostic(cde::DiagnosticCode::UnsupportedLowering, "Class field has no checked storage index",
		                           { { .span = field.name.getSpan(), .text = "expected a stored class field" } });
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
		storage.nominalViews.push_back(*view);
		const cloth::GenericInstance *instance = cp_program.typeInstance(*view);
		const cloth::ResultTypes     &parents  = (instance ? instance->signature : cp_program.declarations.at(view->declaration)).parents;
		const auto                base    = std::ranges::find_if(parents, &cloth::ResolvedType::classReference);
		view                              = base == parents.end() ? nullptr : &*base;
	}
	if (const auto table = cp_program.dispatchTables.find(type); table != cp_program.dispatchTables.end()) {
		storage.nominalViews.insert(storage.nominalViews.end(), table->second.interfaces.begin(), table->second.interfaces.end());
	}
	for (const auto &view : storage.nominalViews) tags.push_back(typeTag(view));
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
	for (const ClassStorage *current = &storage; current; current = current->base) {
		if (current->destructor) {
			LLVMValueRef       self       = callback(storage.destroy, "cloth.destroy", LLVMInt64TypeInContext(context));
			const cloth::GenericInstance *instance =
			        current->instance ?
			                &cp_program.instances.at({ .declaration = current->destructor, .receiver = current->type, .arguments = {} }) :
			                nullptr;
			const LLVMValueRef destructor = functions.declareFunction(*current->destructor, instance);
			LLVMBuildCall2(builder.get(), LLVMGlobalGetValueType(destructor), destructor, &self, 1, "");
		}
		for (const cloth::Symbol *field : current->fields) {
			const cloth::ResolvedType &type = fieldType(*current, *field);
			if (!vt_types.ownsBuffers(type)) continue;
			const LLVMValueRef data = callback(storage.dispose, "cloth.dispose", pointer);
			LLVMValueRef       slot = fieldAddress(builder.get(), data, *current, *field);
			const LLVMValueRef dispose = vt_types.bufferDisposer(type);
			LLVMBuildCall2(builder.get(), LLVMGlobalGetValueType(dispose), dispose, &slot, 1, "");
		}
	}
	for (const LLVMValueRef function : { storage.destroy, storage.dispose }) {
		if (!function) continue;
		LLVMPositionBuilderAtEnd(builder.get(), LLVMGetEntryBasicBlock(function));
		LLVMBuildRetVoid(builder.get());
	}
}

LLVMValueRef ClassObjects::create (LLVMBuilderRef builder, const cloth::ResolvedType &type, ModuleFunctions &functions) {
	ClassStorage &storage = layout(type);
	if (!storage.views) describe(type, storage, functions);
	const LLVMContextRef    context = LLVMGetModuleContext(mod_module);
	const LLVMTargetDataRef target  = LLVMGetModuleDataLayout(mod_module);
	const LLVMTypeRef       word    = LLVMIntPtrTypeInContext(context, target);
	const LLVMValueRef      absent  = LLVMConstNull(LLVMPointerTypeInContext(context, 0));
	const LLVMValueRef      self    = objectCall(builder, mod_module, ObjectOperation::Create,
	                                             { LLVMConstInt(word, LLVMABISizeOfType(target, storage.storage), false),
	                                               LLVMConstInt(word, LLVMABIAlignmentOfType(target, storage.storage), false), storage.views,
	                                               storage.destroy ? storage.destroy : absent, storage.dispose ? storage.dispose : absent });
	objectCall(builder, mod_module, ObjectOperation::Configure, { self, functions.dispatch.descriptor(type) });
	return self;
}
}
