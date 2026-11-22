// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/source_unit.hpp>
#include <token/token.hpp>

namespace cloth {

struct ClassFieldDeclaration;
struct ClassParameterDeclaration;
struct EnumCaseDeclaration;
struct EnumFieldDeclaration;
struct PrimaryConstructorDeclaration;
struct StateAttachmentDeclaration;
struct StateBodyDeclaration;
struct StateInitializerDeclaration;
struct StructFieldDeclaration;

enum class SymbolKind {
	Function,
	Type,
	Field,
	Method,
	Operator,
	Constructor,
	Destructor,
	EnumCase,
	State,
	StateAttachment,
	StateMethodContract,
	StateInitializer,
};

enum class Visibility { Private, Public };

using SymbolDeclaration = std::variant<const CallableSignature *, const TypeDeclaration *, const StructFieldDeclaration *, const ClassFieldDeclaration *,
                                       const ClassParameterDeclaration *, const EnumFieldDeclaration *, const EnumCaseDeclaration *,
                                       const StateBodyDeclaration *, const StateAttachmentDeclaration *, const StateInitializerDeclaration *>;

struct Symbol;
// Keys are NFC identifier spellings. A lookup returns every candidate for that name.
using SymbolScope = std::map<std::string, std::vector<Symbol>, std::less<>>;

struct Symbol {
	SymbolKind        kind;
	Visibility        visibility;
	Token             name;
	SymbolDeclaration declaration;
	SymbolScope       members;
	// Initialization order within the declaring class, starting with parameter fields.
	std::optional<std::size_t> classFieldIndex = std::nullopt;
	// For EnumCase, the zero-based position among all cases of its declaring enum.
	std::size_t enumCaseOrdinal = 0;
};

// Owns the syntax backing every declaration reference, including synthesized primary constructors.
class SymbolTable {
    public:
	explicit SymbolTable (SourceUnit source);

	[[nodiscard]] const SourceUnit &get_source_unit () const {
		return su_source;
	}

	[[nodiscard]] const SymbolScope &get_globals () const {
		return ss_globals;
	}

    private:
	SourceUnit                                                        su_source;
	std::vector<std::shared_ptr<const PrimaryConstructorDeclaration>> v_primaryConstructors;
	SymbolScope                                                       ss_globals;

	void collectType (SymbolScope &scope, const TypeDeclarationPtr &type);
};

}
