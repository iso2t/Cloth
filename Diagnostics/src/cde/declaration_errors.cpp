// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>

#include <cde/declaration_errors.hpp>

namespace cde::DeclarationErrors {

ClothDiagnostic duplicate (const cloth::Token &first, const cloth::Token &repeated) {
	return { DiagnosticCode::DUPLICATE_DECLARATION,
		 std::format("'{}' is declared more than once in this scope", repeated.get_lexeme()),
		 { { repeated.get_span(), "redeclared here" }, { first.get_span(), "first declared here" } } };
}

}
