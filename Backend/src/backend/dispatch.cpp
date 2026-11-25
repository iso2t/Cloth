// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/class_objects.hpp>
#include <backend/dispatch.hpp>
#include <backend/function_abi.hpp>
#include <backend/growable_array.hpp>
#include <backend/nullable_value.hpp>
#include <backend/runtime.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace backend::detail {
namespace {
LLVMValueRef constant (LLVMModuleRef module, LLVMTypeRef type, const char *name, LLVMValueRef value) {
	const LLVMValueRef global = LLVMAddGlobal(module, type, name);
	LLVMSetInitializer(global, value);
	LLVMSetGlobalConstant(global, true);
	LLVMSetLinkage(global, LLVMInternalLinkage);
	return global;
}

LLVMValueRef methodArray (LLVMModuleRef module, LLVMTypeRef pointer, std::vector<LLVMValueRef> &methods) {
	if (methods.empty()) return LLVMConstNull(pointer);
	const LLVMValueRef array = LLVMConstArray2(pointer, methods.data(), methods.size());
	return constant(module, LLVMTypeOf(array), "cloth.methods", array);
}
}

NativeDispatch::NativeDispatch (LLVMModuleRef module) : mod_module(module) {
}

LLVMValueRef NativeDispatch::descriptor (const cloth::ResolvedType &type) {
	if (const auto found = m_tables.find(type); found != m_tables.end()) return found->second.descriptor;
	const LLVMContextRef context        = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    ptr            = LLVMPointerTypeInContext(context, 0);
	LLVMTypeRef          fields[]       = { ptr, ptr, ptr };
	const LLVMTypeRef    descriptorType = LLVMStructTypeInContext(context, fields, 3, false);
	const LLVMValueRef   global         = constant(mod_module, descriptorType, "cloth.dispatch", LLVMConstNull(descriptorType));
	m_tables.emplace(type, ClassDispatchTable{ .descriptor = global, .methods = {}, .states = {} });
	return global;
}

LLVMValueRef NativeDispatch::stateTag (const cloth::Symbol &selection) {
	if (const auto found = m_stateTags.find(&selection); found != m_stateTags.end()) return found->second;
	const LLVMTypeRef  byte = LLVMInt8TypeInContext(LLVMGetModuleContext(mod_module));
	const LLVMValueRef tag  = constant(mod_module, byte, "cloth.state.tag", LLVMConstNull(byte));
	m_stateTags.emplace(&selection, tag);
	return tag;
}

LLVMValueRef NativeDispatch::method (LLVMBuilderRef builder, const cloth::CallDispatch &call, const cloth::ResultTypes &arguments, LLVMValueRef self,
                                     bool directSelf) {
	return lookup(builder, { .call = call, .arguments = arguments }, self, directSelf);
}

LLVMValueRef NativeDispatch::lookup (LLVMBuilderRef builder, const DispatchSlot &request, LLVMValueRef self, bool directSelf) {
	const auto &call  = request.call;
	const auto  found = std::ranges::find_if(v_slots, [&] (const DispatchSlot &slot) {
		return slot.call.kind == call.kind && slot.call.receiver == call.receiver && slot.call.contract == call.contract &&
		       slot.arguments == request.arguments && slot.comparison == request.comparison;
	});
	const auto  index = static_cast<std::size_t>(found - v_slots.begin());
	if (found == v_slots.end()) v_slots.push_back(request);
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	return objectCall(builder, mod_module, ObjectOperation::Method,
	                  { self, LLVMConstInt(LLVMIntPtrTypeInContext(context, LLVMGetModuleDataLayout(mod_module)), index, false),
	                    LLVMConstInt(LLVMInt8TypeInContext(context), directSelf, false) });
}

LLVMValueRef NativeDispatch::compare (LLVMBuilderRef builder, const cloth::ResolvedType &view, cloth::Operator op, LLVMValueRef self, LLVMValueRef peer,
                                      bool directSelf) {
	const LLVMValueRef function =
	        lookup(builder,
	               { .call       = { .kind = cloth::DispatchKind::Direct, .receiver = view, .contract = { .symbol = nullptr, .owner = view } },
	                 .arguments  = {},
	                 .comparison = op },
	               self, directSelf);
	LLVMTypeRef  types[]  = { LLVMTypeOf(self), LLVMTypeOf(peer) };
	LLVMValueRef inputs[] = { self, peer };
	return LLVMBuildCall2(builder, LLVMFunctionType(LLVMInt8TypeInContext(LLVMGetModuleContext(mod_module)), types, 2, false), function, inputs, 2, "");
}

LLVMValueRef NativeDispatch::comparisonBody (ModuleFunctions &functions, const cloth::TypeDispatch &checked, cloth::Operator op) {
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    handle = LLVMInt64TypeInContext(context), byte = LLVMInt8TypeInContext(context);
	LLVMTypeRef          parameters[] = { handle, handle };
	const LLVMValueRef   function     = LLVMAddFunction(mod_module, "cloth.reference.compare", LLVMFunctionType(byte, parameters, 2, false));
	LLVMSetLinkage(function, LLVMInternalLinkage);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> builder(LLVMCreateBuilderInContext(context), LLVMDisposeBuilder);
	LLVMPositionBuilderAtEnd(builder.get(), LLVMAppendBasicBlockInContext(context, function, "entry"));
	const LLVMValueRef self = LLVMGetParam(function, 0), peer = LLVMGetParam(function, 1);
	const auto         matchesPeer = [&] (const cloth::ResolvedType &parameter) {
		const auto         type = parameter.nullable() ? parameter.element() : parameter;
		const LLVMValueRef matches =
		        objectCall(builder.get(), mod_module, ObjectOperation::Is, { peer, functions.objects.typeTag(type), LLVMConstInt(byte, 1, false) });
		return LLVMBuildIsNotNull(builder.get(), matches, "");
	};
	for (const auto candidate : { cloth::ONotEqual, cloth::OEqual }) {
		if (candidate == cloth::ONotEqual && op == cloth::OEqual) continue;
		for (const auto &candidateBody : checked.referenceEquality.at(candidate)) {
			const auto             &parameter = candidateBody.parameter;
			const LLVMBasicBlockRef invoke    = LLVMAppendBasicBlockInContext(context, function, "matched"),
			                        next      = LLVMAppendBasicBlockInContext(context, function, "next");
			LLVMBuildCondBr(builder.get(), matchesPeer(parameter), invoke, next);
			LLVMPositionBuilderAtEnd(builder.get(), invoke);
			if (!candidateBody.competingPeers.empty()) {
				LLVMValueRef tie = LLVMConstNull(LLVMInt1TypeInContext(context));
				for (const auto &other : candidateBody.competingPeers) tie = LLVMBuildOr(builder.get(), tie, matchesPeer(other), "");
				const LLVMBasicBlockRef ambiguous = LLVMAppendBasicBlockInContext(context, function, "ambiguous"),
				                        unique    = LLVMAppendBasicBlockInContext(context, function, "unique");
				LLVMBuildCondBr(builder.get(), tie, ambiguous, unique);
				LLVMPositionBuilderAtEnd(builder.get(), ambiguous);
				const LLVMValueRef fail = declareAmbiguousEquality(mod_module);
				LLVMBuildCall2(builder.get(), LLVMGlobalGetValueType(fail), fail, nullptr, 0, "");
				LLVMBuildUnreachable(builder.get());
				LLVMPositionBuilderAtEnd(builder.get(), unique);
			}
			const LLVMValueRef body     = functions.declareMember(candidateBody.method);
			LLVMValueRef       inputs[] = { self, parameter.nullable() ? nullable::pack(builder.get(), peer) : peer };
			LLVMValueRef       result   = LLVMBuildCall2(builder.get(), LLVMGlobalGetValueType(body), body, inputs, 2, "");
			if (candidate != op) result = LLVMBuildZExt(builder.get(), LLVMBuildIsNull(builder.get(), result, ""), byte, "");
			LLVMBuildRet(builder.get(), result);
			LLVMPositionBuilderAtEnd(builder.get(), next);
		}
	}
	LLVMBuildRet(builder.get(),
	             LLVMBuildZExt(builder.get(), LLVMBuildICmp(builder.get(), op == cloth::OEqual ? LLVMIntEQ : LLVMIntNE, self, peer, ""), byte, ""));
	return function;
}

void NativeDispatch::resolve (ModuleFunctions &functions) {
	const LLVMValueRef absent = LLVMConstNull(LLVMPointerTypeInContext(LLVMGetModuleContext(mod_module), 0));
	for (auto &[type, table] : m_tables) {
		const auto &checked = functions.program.dispatchTables.at(type);
		for (const auto &[name, state] : checked.states) table.states.try_emplace(state.selection);
		while (table.methods.size() < v_slots.size()) {
			const auto  &slot           = v_slots[table.methods.size()];
			const auto  &views          = functions.objects.layout(type).nominalViews;
			const bool   compatible     = std::ranges::find(views, slot.call.receiver) != views.end();
			LLVMValueRef implementation = absent;
			if (compatible && slot.comparison) {
				auto &callback = table.comparisons[*slot.comparison];
				if (!callback) callback = comparisonBody(functions, checked, *slot.comparison);
				implementation = callback;
			} else if (const auto *body = compatible ? checked.methodTarget(slot.call) : nullptr) {
				implementation = functions.declareMember(*body, slot.arguments);
			}
			table.methods.push_back(implementation);
			const auto *stateMethod = compatible && slot.call.kind == cloth::DispatchKind::State ? checked.stateTarget(slot.call.contract) :
			                                                                                       nullptr;
			for (auto &[selection, row] : table.states) {
				LLVMValueRef method = absent;
				if (stateMethod) {
					const auto found = stateMethod->implementations.find(selection);
					if (found != stateMethod->implementations.end()) method = functions.declareMember(found->second, slot.arguments);
				}
				row.push_back(method);
			}
		}
	}
}

void NativeDispatch::finish (const cloth::CheckedProgram &program) {
	const LLVMContextRef context       = LLVMGetModuleContext(mod_module);
	const LLVMTypeRef    ptr           = LLVMPointerTypeInContext(context, 0);
	LLVMTypeRef          stateFields[] = { ptr, ptr };
	const LLVMTypeRef    stateType     = LLVMStructTypeInContext(context, stateFields, 2, false);
	for (auto &[type, table] : m_tables) {
		std::vector<LLVMValueRef> states;
		unsigned                  initial  = 0;
		const auto               &checked  = program.dispatchTables.at(type);
		const auto               *selected = checked.initialState;
		for (const auto &[name, state] : checked.states) {
			const auto *selection = state.selection;
			auto       &row       = table.states.at(selection);
			if (selection == selected) initial = static_cast<unsigned>(states.size());
			LLVMValueRef fields[] = { stateTag(*selection), methodArray(mod_module, ptr, row) };
			states.push_back(LLVMConstStructInContext(context, fields, 2, false));
		}
		states.push_back(LLVMConstNull(stateType));
		const LLVMValueRef values    = LLVMConstArray2(stateType, states.data(), states.size());
		const LLVMValueRef stateList = constant(mod_module, LLVMTypeOf(values), "cloth.states", values);
		const LLVMTypeRef  i32       = LLVMInt32TypeInContext(context);
		LLVMValueRef       path[]    = { LLVMConstNull(i32), LLVMConstInt(i32, initial, false) };
		LLVMValueRef       fields[]  = { methodArray(mod_module, ptr, table.methods), stateList,
			                         selected ? LLVMConstInBoundsGEP2(LLVMTypeOf(values), stateList, path, 2) : LLVMConstNull(ptr) };
		LLVMSetInitializer(table.descriptor, LLVMConstStructInContext(context, fields, 3, false));
	}
}

LLVMValueRef NativeDispatch::payloadDisposer (ModuleFunctions &functions, const cloth::ResolvedType &type) {
	const LLVMContextRef context = LLVMGetModuleContext(mod_module);
	LLVMTypeRef          pointer = LLVMPointerTypeInContext(context, 0);
	if (!growable_array::ownsBuffer(type)) return LLVMConstNull(pointer);
	if (const auto found = m_disposers.find(type); found != m_disposers.end()) return found->second;
	const LLVMValueRef function =
	        LLVMAddFunction(mod_module, "cloth.dispose.payload", LLVMFunctionType(LLVMVoidTypeInContext(context), &pointer, 1, false));
	LLVMSetLinkage(function, LLVMInternalLinkage);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> builder(LLVMCreateBuilderInContext(context), LLVMDisposeBuilder);
	LLVMPositionBuilderAtEnd(builder.get(), LLVMAppendBasicBlockInContext(context, function, "entry"));
	LLVMValueRef slot = LLVMGetParam(function, 0);
	if (type.nullable()) slot = nullable::payloadAddress(builder.get(), functions.valueTypes.localType(type.element()), slot);
	growable_array::release(builder.get(), mod_module, slot);
	LLVMBuildRetVoid(builder.get());
	m_disposers.emplace(type, function);
	return function;
}
}
