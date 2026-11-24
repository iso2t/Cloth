// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/import_declaration.hpp>
#include <file/source_file.hpp>
#include <token/token.hpp>

namespace cloth {

struct SourceUnit {
	std::vector<DeclarationPtr>    declarations;
	std::optional<TokenSpan>       end     = std::nullopt;
	std::vector<ImportDeclaration> imports = {};

	[[nodiscard]] const SourceFile *getSource () const {
		return end ? end->source.get() : nullptr;
	}
};

} // namespace cloth
