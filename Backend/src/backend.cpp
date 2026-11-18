// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend.hpp>

namespace backend {

BackendObject::BackendObject (const cloth::SourceUnit &ast) {
	this->ast = ast;
}

}
