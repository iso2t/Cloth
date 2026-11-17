// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <file/source_file.hpp>
#include <parser/decl/declarations.hpp>
#include <parser/decl/import_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

struct SourceUnit {
	std::vector<DeclarationPtr>    declarations;
	std::optional<TokenSpan>       end     = std::nullopt;
	std::vector<ImportDeclaration> imports = {};

	const SourceFile *get_source () const {
		return end ? end->source.get() : nullptr;
	}
};

} // namespace cloth
