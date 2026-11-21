// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>

#include <llvm-c/Core.h>

namespace cloth {
struct CheckedProgram;
}

namespace backend {

class LoweredModule {
    public:
	explicit LoweredModule (const cloth::CheckedProgram &program);

	// The LLVM handle is valid for the lifetime of this object.
	[[nodiscard]] LLVMModuleRef get () const;
	void                        verify () const;

    private:
	// The module borrows its context and must be destroyed first.
	const std::unique_ptr<LLVMOpaqueContext, decltype(&LLVMContextDispose)> ctx_context;
	const std::unique_ptr<LLVMOpaqueModule, decltype(&LLVMDisposeModule)>   mod_module;
};

}
