// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <semantic/checked_program.hpp>

namespace backend {

class BackendObject {
    public:
	explicit BackendObject (cloth::CheckedProgram program);
	~BackendObject () = default;

    private:
	cloth::CheckedProgram cp_program;
};

}
