// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <diagnostics.hpp>
#include <token/token.hpp>

namespace cde {

namespace {

std::vector<SourceLabel> withPrimaryLabel (cloth::TokenSpan span, std::vector<SourceLabel> related) {
	related.insert(related.begin(), SourceLabel{ .span = std::move(span), .text = "here" });
	return related;
}

}

SyntaxException::SyntaxException (const DiagnosticCode code, std::string message, cloth::TokenSpan span, std::optional<std::string> guidance,
                                  std::vector<SourceLabel> related) :
        ClothDiagnostic(code, std::move(message), withPrimaryLabel(std::move(span), std::move(related)), std::move(guidance)) {
}

}
