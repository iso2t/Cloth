// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <utility>

#include <backend.hpp>

namespace backend {

BackendObject::BackendObject (cloth::CheckedProgram program) : cp_program(std::move(program)) {
}

}
