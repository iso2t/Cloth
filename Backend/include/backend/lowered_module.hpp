// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

namespace cloth {
struct CheckedProgram;
}

namespace backend {

class LoweredModule {
    public:
	explicit LoweredModule (const cloth::CheckedProgram &program, std::string_view targetTriple = {});

	// The LLVM handle is valid for the lifetime of this object.
	[[nodiscard]] LLVMModuleRef          get () const;
	void                                 verify () const;
	[[nodiscard]] std::vector<std::byte> emitObject ();

    private:
	// The module borrows its context and must be destroyed first.
	const std::unique_ptr<LLVMOpaqueContext, decltype(&LLVMContextDispose)>             ctx_context;
	const std::unique_ptr<LLVMOpaqueTargetMachine, decltype(&LLVMDisposeTargetMachine)> tm_machine;
	const std::unique_ptr<LLVMOpaqueModule, decltype(&LLVMDisposeModule)>               mod_module;
};

}
