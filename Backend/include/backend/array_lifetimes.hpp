// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <vector>

#include <llvm-c/Types.h>

namespace backend::detail {

class ArrayLifetimes {
    public:
	enum class Kind;
	struct Mark;

	ArrayLifetimes (LLVMModuleRef module, LLVMBuilderRef builder);
	[[nodiscard]] Mark mark () const;
	LLVMValueRef       own (LLVMTypeRef descriptor, Kind kind, bool nullable = false);
	// A jump cleans up its path while sibling paths can still use the same active scope.
	void releaseSince (Mark scope) const;
	void endScope (Mark scope);
	void endStatement (Mark scope);

    private:
	struct Owner;

	void release (Mark scope, bool temporariesOnly) const;

	LLVMModuleRef      mod_module;
	LLVMBuilderRef     ir_builder;
	std::vector<Owner> v_owners;
	std::size_t        st_locals = 0;
};

enum class ArrayLifetimes::Kind { Local, Temporary };

struct ArrayLifetimes::Mark {
	std::size_t locals{};
	std::size_t temporaries{};
};

struct ArrayLifetimes::Owner {
	LLVMValueRef slot;
	Kind         kind;
	LLVMTypeRef  nullablePayload;
};
}
