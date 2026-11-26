// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/import_declaration.hpp>
#include <file/source_file.hpp>
#include <token/token.hpp>

namespace cloth {

enum class SourceGrammar { Legacy, ExplicitSemicolons };
inline constexpr SourceGrammar DEFAULT_SOURCE_GRAMMAR = SourceGrammar::ExplicitSemicolons;

struct SourceUnit {
	std::vector<DeclarationPtr>    declarations;
	std::optional<TokenSpan>       end     = std::nullopt;
	std::vector<ImportDeclaration> imports = {};
	SourceGrammar                  grammar = DEFAULT_SOURCE_GRAMMAR;

	[[nodiscard]] const SourceFile *getSource () const {
		return end ? end->source.get() : nullptr;
	}
};

} // namespace cloth
