// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>

#include <diagnostics.hpp>
#include <token/token.hpp>

namespace cde::DeclarationErrors {

ClothDiagnostic duplicate (const cloth::Token &first, const cloth::Token &repeated) {
	return { DiagnosticCode::DuplicateDeclaration,
		 std::format("'{}' is declared more than once in this scope", repeated.getLexeme()),
		 { { .span = repeated.getSpan(), .text = "redeclared here" }, { .span = first.getSpan(), .text = "first declared here" } } };
}

}
