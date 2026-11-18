// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>

#include <cde/import_errors.hpp>

namespace cde::ImportErrors {

SyntaxException outsideHeader (const cloth::Token &keyword) {
	return { DiagnosticCode::IMPORT_OUTSIDE_HEADER,
		 std::format("'{}' imports must appear in the file header before ordinary declarations", cloth::to_string(cloth::Keyword::kUsing)),
		 keyword.get_span() };
}

SyntaxException duplicateSelection (const cloth::Token &first, const cloth::Token &repeated) {
	return { DiagnosticCode::DUPLICATE_IMPORT_SELECTION,
		 std::format("Import selection '{}' is repeated", repeated.get_lexeme()),
		 repeated.get_span(),
		 std::nullopt,
		 { { first.get_span(), "First selection here" } } };
}

}
