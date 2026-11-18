// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ast/source_unit.hpp>

namespace backend {

class BackendObject {
    public:
	explicit BackendObject (const cloth::SourceUnit &ast);
	~BackendObject () = default;

    private:
	cloth::SourceUnit ast;
};

}
