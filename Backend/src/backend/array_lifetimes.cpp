// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <backend/array_lifetimes.hpp>
#include <backend/growable_array.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <span>

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

namespace backend::detail {

ArrayLifetimes::ArrayLifetimes (LLVMModuleRef module, LLVMBuilderRef builder) : mod_module(module), ir_builder(builder) {
}

ArrayLifetimes::Mark ArrayLifetimes::mark () const {
	return { .locals = st_locals, .temporaries = v_owners.size() - st_locals };
}

LLVMValueRef ArrayLifetimes::own (LLVMTypeRef descriptor, Kind kind) {
	// Cleanup after a branch can reach a slot whose source expression was skipped.
	const LLVMValueRef ir_slot = allocateEntrySlot(ir_builder, descriptor, LLVMConstNull(descriptor));
	v_owners.push_back({ .slot = ir_slot, .kind = kind });
	if (kind == Kind::Local) ++st_locals;
	return ir_slot;
}

void ArrayLifetimes::release (Mark scope, bool temporariesOnly) const {
	if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ir_builder))) return;
	const std::span<const Owner> owners = std::span(v_owners).subspan(scope.locals + scope.temporaries);
	for (const Owner &owner : std::views::reverse(owners)) {
		if (!temporariesOnly || owner.kind == Kind::Temporary) growable_array::release(ir_builder, mod_module, owner.slot);
	}
}

void ArrayLifetimes::releaseSince (Mark scope) const {
	release(scope, false);
}

void ArrayLifetimes::endScope (Mark scope) {
	releaseSince(scope);
	v_owners.resize(scope.locals + scope.temporaries);
	st_locals = scope.locals;
}

void ArrayLifetimes::endStatement (Mark scope) {
	if (mark().temporaries == scope.temporaries) return;
	release(scope, true);
	const auto it_first = v_owners.begin() + static_cast<std::ptrdiff_t>(scope.locals + scope.temporaries);
	const auto removed  = std::ranges::remove(it_first, v_owners.end(), Kind::Temporary, &Owner::kind);
	v_owners.erase(removed.begin(), removed.end());
}
}
