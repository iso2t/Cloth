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

	TopLevelStateDeclaration parseTopLevelState (const Token &typedefKeyword, const Token &keyword, TypeReference payload);
	ClassDeclaration         parse (const Token &keyword, std::optional<Token> visibility, std::optional<Token> finalKeyword);

    private:
	Parser &p_parser;

	ClassParameterDeclaration   parseHeaderParameter ();
	std::vector<ClassMember>    parseMembers (const Token &owner, std::vector<TypeDeclarationPtr> &nestedTypes);
	ClassMember                 parseMember (const Token &owner);
	InitialStatePrefix          parseInitialState ();
	StateClassMemberPtr         parseState (std::optional<Token> visibility, std::optional<InitialStatePrefix> initial);
	std::vector<StateMemberPtr> parseStateBody (std::vector<TypeDeclarationPtr> *nestedTypes = nullptr);
	void                        finishMember (bool allowsTerminator, Keyword kind);
	ClassFieldDeclaration       parseField (std::optional<Token> visibility);
	TypeReference               parseFieldType ();
	ConstructorDeclaration      parseConstructor (const Token &owner);
	DestructorDeclaration       parseDestructor (const Token &owner);
	Token                       parseMemberName (const Token &owner, std::string_view kind);
	std::optional<Token>        parseVisibility ();
	bool                        at (Operator op);
};

}
