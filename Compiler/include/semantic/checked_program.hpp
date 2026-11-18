// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/type/type_parameters.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth {

struct BinaryExpression;
struct TypeCheckExpression;

struct ResolvedType {
	std::string                     name;
	const Symbol                   *declaration = nullptr;
	const TypeParameterDeclaration *parameter   = nullptr;
	std::vector<ResolvedType>       arguments;
	// Suffixes are ordered from the element outward, so int?[] differs from int[]?.
	std::vector<std::string> suffixes;
	unsigned                 width = 0;

	explicit ResolvedType (std::string name = {});
	explicit ResolvedType (Keyword keyword);
	bool                       operator==(const ResolvedType &) const = default;
	[[nodiscard]] bool         nullable () const;
	[[nodiscard]] bool         array () const;
	[[nodiscard]] bool         integer () const;
	[[nodiscard]] bool         floating () const;
	[[nodiscard]] ResolvedType element () const;
	[[nodiscard]] std::string  spelling () const;
};

using ResultTypes   = std::vector<ResolvedType>;
using Substitutions = std::map<const TypeParameterDeclaration *, ResolvedType>;

struct DeclarationInfo {
	std::size_t                 file;
	const Symbol               *owner;
	ResultTypes                 parameters    = {};
	ResultTypes                 returns       = {};
	ResultTypes                 parents       = {};
	std::optional<ResolvedType> value         = {};
	const Symbol               *attachedState = nullptr;
	bool                        inferResult   = false;
};

struct ExpressionInfo {
	ResultTypes              types;
	const Symbol            *symbol               = nullptr;
	const Token             *binding              = nullptr;
	std::vector<std::size_t> parameterForArgument = {};
	bool                     fallsThrough         = true;
	Substitutions            typeArguments        = {};
};

struct CheckedProgram {
	explicit CheckedProgram (SymbolTable entry);

	// Keep the parsed units and generated declarations alive for the semantic results.
	std::vector<std::unique_ptr<const SymbolTable>>        units;
	std::deque<Symbol>                                     constructorRequirements;
	std::unordered_map<const Symbol *, DeclarationInfo>    declarations;
	std::unordered_map<const Expression *, ExpressionInfo> expressions;
	std::unordered_map<const Token *, ResolvedType>        localTypes;
	// An "is" target can resolve to a value after parsing. Keep that expression alive with its bindings.
	std::unordered_map<const TypeCheckExpression *, std::shared_ptr<const BinaryExpression>> valueTests;
	std::unordered_map<const Statement *, ExpressionInfo>                                    writes;
	std::unordered_map<const Token *, const Token *>                                         references;
};

}
