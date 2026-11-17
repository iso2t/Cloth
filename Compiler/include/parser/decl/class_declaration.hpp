// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/decl/state_declaration.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <parser/expr/class_expression.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/type/type_parameters.hpp>
#include <parser/type/type_reference.hpp>
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

	TokenSpan get_span () const {
		const std::optional<Token> &opt_modifier = visibility ? visibility : fieldKeyword;
		return opt_modifier ? TokenSpan::span_through(opt_modifier->get_span(), parameter.span) : parameter.span;
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

	ConstructorDeclaration (const Token &name, std::vector<ParameterDeclaration> parameters, ConstructorDelegation delegation, BlockBody body);

	std::vector<TypeReference> get_return_types () const {
		return {};
	}
};

struct DestructorDeclaration : CallableDeclaration {
	DestructorDeclaration (const Token &op, const Token &name, BlockBody body);

	const Token &get_operator () const {
		return keyword;
	}

	std::vector<TypeReference> get_return_types () const {
		return {};
	}
};

struct ClassMember {
	std::variant<ClassFieldDeclaration, ConstructorDeclaration, DestructorDeclaration, StateClassMemberPtr, MethodDeclaration> value;

	TokenSpan get_span () const {
		return std::visit(
		        []<typename Member>(const Member &member) -> TokenSpan {
			        if constexpr (std::is_same_v<Member, StateClassMemberPtr>) {
				        return member->get_span();
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

	std::vector<TypeReference> get_return_types () const {
		return {};
	}
};

}
