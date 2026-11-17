// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/import_declaration.hpp>
#include <token/token.hpp>

namespace cloth {

struct SourceUnit {
	std::vector<Declaration>       declarations;
	std::optional<TokenSpan>       span = std::nullopt;
	std::vector<ImportDeclaration> imports;
};

} // namespace cloth
