// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>

#include <ast/decl/declarations.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_parameters.hpp>
#include <semantic/checker.hpp>

namespace cloth::semantic {
bool Analyzer::consoleOverload (const Symbol &symbol) const {
	const auto &name = symbol.name.getNameKey();
	if (symbol.kind != SymbolKind::Function || symbol.visibility != Visibility::Public || (name != "print" && name != "println" && name != "input"))
		return false;
	const auto &decl  = declarations.at(&symbol);
	const auto  paths = m_libraryPaths.find(decl.file);
	if (paths == m_libraryPaths.end() || !paths->second.contains(CONSOLE_MODULE)) return false;
	if (name == "input") {
		if (decl.returns.size() != 1 || !decl.returns.front().nullable() || decl.returns.front().element() != ResolvedType(KString)) return false;
	} else if (!decl.returns.empty())
		return false;
	const auto *typeParams = typeParameters(symbol);
	if (decl.parameters.empty()) return !typeParams && name != "print";
	if (!typeParams || typeParams->parameters.size() != 1 || decl.parameters.size() != 1) return false;
	const auto &parameter = decl.parameters.front();
	return parameter.nullable() && parameter.suffixes.size() == 1 && parameter.parameter == &typeParams->parameters.front() &&
	       !callable(symbol)->parameters.front().isOptional();
}

void BodyChecker::checkConsoleText (const Token &name, const Expression &argument, const ResolvedType &type) {
	// Record the access at the caller for lifetime and default-argument diagnostics.
	if (!bindText(argument, type))
		fail(Code::InvalidBuiltinArgument, argument.getSpan(),
		     std::format("'{}' requires a printable value, got {}", name.getLexeme(), type.spelling()));
}

}
