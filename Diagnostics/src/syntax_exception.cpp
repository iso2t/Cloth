// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <utility>

#include <syntax_exception.hpp>

namespace cde {

namespace {

std::vector<SourceLabel> with_primary_label (cloth::TokenSpan span, std::vector<SourceLabel> related) {
	related.insert(related.begin(), SourceLabel{ span, "here" });
	return related;
}

}

SyntaxException::SyntaxException (DiagnosticCode code, std::string message, cloth::TokenSpan span, std::optional<std::string> guidance,
                                  std::vector<SourceLabel> related) :
        ClothDiagnostic(code, std::move(message), with_primary_label(span, std::move(related)), std::move(guidance)) {
}

}
