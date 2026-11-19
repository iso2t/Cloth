// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>

#include <utility>

namespace backend {

BackendObject::BackendObject (cloth::CheckedProgram program) : cp_program(std::move(program)) {
}

}
