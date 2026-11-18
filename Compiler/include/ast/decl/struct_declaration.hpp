// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct StructFieldDeclaration {
	TypeReference        type;
	Token                name;
	std::optional<Token> visibility = std::nullopt;

	[[nodiscard]] TokenSpan get_span () const {
		return TokenSpan::span_through(visibility ? visibility->get_span() : type.get_span(), name.get_span());
	}
};

struct OperatorMember {
	std::optional<TypeReference> selector;
	bool                         rightReceiver;
	bool                         provider;
};

struct MethodDeclaration : CallableDeclaration, StateMember {
	std::optional<OperatorMember>    operatorMember;
	std::optional<TokenSpan>         overridePrefix;
	std::optional<TokenSpan>         virtualPrefix;
	std::optional<TypeParameterList> typeParameters;

	MethodDeclaration (const Token &keyword, const Token &name, std::vector<TypeReference> returnTypes, std::vector<ParameterDeclaration> parameters,
	                   FunctionBody body, std::optional<Token> visibility = std::nullopt, std::optional<OperatorMember> operatorMember = std::nullopt,
	                   const std::optional<TokenSpan> &overridePrefix = std::nullopt, const std::optional<TokenSpan> &virtualPrefix = std::nullopt,
	                   std::optional<TypeParameterList> typeParameters = std::nullopt) :
	        CallableDeclaration(keyword, name, std::move(parameters), std::move(body),
	                            virtualPrefix.value_or(overridePrefix.value_or((visibility ? *visibility : keyword).get_span())), std::move(returnTypes),
	                            visibility),
	        operatorMember(std::move(operatorMember)),
	        overridePrefix(overridePrefix),
	        virtualPrefix(virtualPrefix),
	        typeParameters(std::move(typeParameters)) {
	}

	[[nodiscard]] TokenSpan get_span () const override {
		return span;
	}
};

struct StructDeclaration : TypeDeclaration {
	Token                               keyword;
	std::vector<StructFieldDeclaration> fields;
	std::vector<MethodDeclaration>      methods;
	std::optional<TypeParameterList>    typeParameters;

	StructDeclaration (const Token &keyword, const Token &name, std::vector<StructFieldDeclaration> fields, std::optional<Token> visibility, TokenSpan span,
	                   std::vector<MethodDeclaration> methods = {}, std::optional<TypeParameterList> typeParameters = std::nullopt,
	                   std::vector<TypeDeclarationPtr> nestedTypes = {}) :
	        TypeDeclaration(name, std::move(visibility), std::move(span), std::move(nestedTypes)),
	        keyword(keyword),
	        fields(std::move(fields)),
	        methods(std::move(methods)),
	        typeParameters(std::move(typeParameters)) {
	}
};

}
