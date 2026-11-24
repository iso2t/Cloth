// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>

#include <diagnostics.hpp>
#include <token/token.hpp>

namespace cde::ImportErrors {

SyntaxException outsideHeader (const cloth::Token &keyword) {
	return { DiagnosticCode::ImportOutsideHeader,
		 std::format("'{}' imports must appear in the file header before ordinary declarations", cloth::toString(cloth::Keyword::KUsing)),
		 keyword.getSpan() };
}

SyntaxException duplicateSelection (const cloth::Token &first, const cloth::Token &repeated) {
	return { DiagnosticCode::DuplicateImportSelection,
		 std::format("Import selection '{}' is repeated", repeated.getLexeme()),
		 repeated.getSpan(),
		 std::nullopt,
		 { { .span=first.getSpan(), .text="First selection here" } } };
}

}
