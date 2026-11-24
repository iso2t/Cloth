// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/class_expression.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

/** An argument list records explicit parentheses, even if the list is empty. */
struct ClassParentClause {
	TypeReference                            type;
	std::optional<std::vector<CallArgument>> constructorArguments;
	TokenSpan                                span;
};

struct ClassParameterDeclaration {
	ParameterDeclaration parameter;
	std::optional<Token> fieldKeyword = std::nullopt;
	std::optional<Token> visibility   = std::nullopt;

	[[nodiscard]] TokenSpan getSpan () const {
		const std::optional<Token> &opt_modifier = visibility ? visibility : fieldKeyword;
		return opt_modifier ? TokenSpan::spanThrough(opt_modifier->getSpan(), parameter.span) : parameter.span;
	}
};

struct ClassFieldDeclaration {
	Token                keyword;
	TypeReference        type;
	Token                name;
	ExpressionPtr        initializer;
	std::optional<Token> visibility;
	TokenSpan            span;
};

struct ConstructorDeclaration : CallableDeclaration {
	ConstructorDelegation delegation;

	ConstructorDeclaration (const Token &name, std::vector<ParameterDeclaration> parameters, ConstructorDelegation delegation, BlockBody body,
	                        const std::optional<Token> &visibility = std::nullopt);

	static std::vector<TypeReference> getReturnTypes () {
		return {};
	}
};

struct DestructorDeclaration : CallableDeclaration {
	DestructorDeclaration (const Token &op, const Token &name, BlockBody body, const std::optional<Token> &visibility = std::nullopt);

	[[nodiscard]] const Token &getOperator () const {
		return keyword;
	}

	static std::vector<TypeReference> getReturnTypes () {
		return {};
	}
};

struct ClassMember {
	std::variant<ClassFieldDeclaration, ConstructorDeclaration, DestructorDeclaration, StateClassMemberPtr, MethodDeclaration> value;

	[[nodiscard]] TokenSpan getSpan () const {
		return std::visit(
		        []<typename Member>(const Member &member) -> TokenSpan {
			        if constexpr (std::is_same_v<Member, StateClassMemberPtr>) {
				        return member->getSpan();
			        } else {
				        return member.span;
			        }
		        },
		        value);
	}
};

struct ClassDeclaration : TypeDeclaration {
	Token                                  keyword;
	std::optional<Token>                   finalKeyword;
	std::vector<ClassParameterDeclaration> parameters;
	std::vector<ClassMember>               members;
	std::vector<ClassParentClause>         parents;
	std::optional<TypeParameterList>       typeParameters;

	ClassDeclaration (const Token &keyword, const Token &name, std::optional<Token> visibility, std::optional<Token> finalKeyword,
	                  std::vector<ClassParameterDeclaration> parameters, std::vector<ClassMember> members, TokenSpan span,
	                  std::vector<ClassParentClause> parents = {}, std::optional<TypeParameterList> typeParameters = std::nullopt,
	                  std::vector<TypeDeclarationPtr> nestedTypes = {});
};

struct PrimaryConstructorDeclaration : CallableDeclaration {
	std::shared_ptr<const ClassDeclaration> owner;

	explicit PrimaryConstructorDeclaration (std::shared_ptr<const ClassDeclaration> owner);

	static std::vector<TypeReference> getReturnTypes () {
		return {};
	}
};

}
