// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cde/cloth_diagnostic.hpp>
#include <token/token.hpp>

namespace cde::DeclarationErrors {

ClothDiagnostic duplicate (const cloth::Token &first, const cloth::Token &repeated);

}
