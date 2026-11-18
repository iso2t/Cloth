// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class ClassParser {
    public:
	explicit ClassParser (Parser &parser) : p_parser(parser) {
	}

	[[nodiscard]] TopLevelStateDeclaration parseTopLevelState (const Token &typedefKeyword, const Token &keyword, TypeReference payload) const;
	[[nodiscard]] ClassDeclaration         parse (const Token &keyword, std::optional<Token> visibility, std::optional<Token> finalKeyword) const;

    private:
	Parser &p_parser;

	[[nodiscard]] ClassParameterDeclaration parseHeaderParameter () const;
	std::vector<ClassMember>                parseMembers (const Token &owner, std::vector<TypeDeclarationPtr> &nestedTypes) const;
	[[nodiscard]] ClassMember               parseMember (const Token &owner) const;
	[[nodiscard]] InitialStatePrefix        parseInitialState () const;
	[[nodiscard]] StateClassMemberPtr       parseState (std::optional<Token> visibility, std::optional<InitialStatePrefix> initial) const;
	std::vector<StateMemberPtr>             parseStateBody (std::vector<TypeDeclarationPtr> *nestedTypes = nullptr) const;
	void                                    finishMember (bool allowsTerminator, Keyword kind) const;
	[[nodiscard]] ClassFieldDeclaration     parseField (std::optional<Token> visibility) const;
	[[nodiscard]] TypeReference             parseFieldType () const;
	[[nodiscard]] ConstructorDeclaration    parseConstructor (const Token &owner, const std::optional<Token> &visibility) const;
	[[nodiscard]] DestructorDeclaration     parseDestructor (const Token &owner, const std::optional<Token> &visibility) const;
	[[nodiscard]] Token                     parseMemberName (const Token &owner, std::string_view kind) const;
	[[nodiscard]] bool                      at (Operator op) const;
};

}
