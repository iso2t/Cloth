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

	TopLevelStateDeclaration parseTopLevelState (const Token &typedefKeyword, const Token &keyword, TypeReference payload) const;
	ClassDeclaration         parse (const Token &keyword, std::optional<Token> visibility, std::optional<Token> finalKeyword) const;

    private:
	Parser &p_parser;

	ClassParameterDeclaration   parseHeaderParameter () const;
	std::vector<ClassMember>    parseMembers (const Token &owner, std::vector<TypeDeclarationPtr> &nestedTypes) const;
	ClassMember                 parseMember (const Token &owner) const;
	InitialStatePrefix          parseInitialState () const;
	StateClassMemberPtr         parseState (std::optional<Token> visibility, std::optional<InitialStatePrefix> initial) const;
	std::vector<StateMemberPtr> parseStateBody (std::vector<TypeDeclarationPtr> *nestedTypes = nullptr) const;
	void                        finishMember (bool allowsTerminator, Keyword kind) const;
	ClassFieldDeclaration       parseField (std::optional<Token> visibility) const;
	TypeReference               parseFieldType () const;
	ConstructorDeclaration      parseConstructor (const Token &owner) const;
	DestructorDeclaration       parseDestructor (const Token &owner) const;
	Token                       parseMemberName (const Token &owner, std::string_view kind) const;
	std::optional<Token>        parseVisibility () const;
	bool                        at (Operator op) const;
};

}
