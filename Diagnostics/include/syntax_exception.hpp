// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <cloth_diagnostic.hpp>
#include <token/token.hpp>

namespace cde {

class SyntaxException final : public ClothDiagnostic {
    public:
	SyntaxException (DiagnosticCode code, std::string message, cloth::TokenSpan span, std::optional<std::string> guidance = std::nullopt,
	                 std::vector<SourceLabel> related = {});
};

}
