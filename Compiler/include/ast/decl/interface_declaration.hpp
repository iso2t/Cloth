// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct InterfaceMethodFallback {
	Token        equals;
	Token        keyword;
	FunctionBody body;
};

struct InterfaceMethodDeclaration : CallableSignature {
	Token                                  name;
	std::optional<InterfaceMethodFallback> fallback;
	std::optional<TypeParameterList>       typeParameters;

	InterfaceMethodDeclaration (const Token &keyword, const Token &name, std::vector<TypeReference> returnTypes,
	                            std::vector<ParameterDeclaration> parameters, std::optional<InterfaceMethodFallback> fallback, const TokenSpan &span,
	                            std::optional<TypeParameterList> typeParameters = std::nullopt) :
	        CallableSignature(keyword, std::move(parameters), span, std::move(returnTypes)),
	        name(name),
	        fallback(std::move(fallback)),
	        typeParameters(std::move(typeParameters)) {
	}

	[[nodiscard]] const Token &get_name () const override {
		return name;
	}

	[[nodiscard]] const FunctionBody *executableBody () const override {
		return fallback ? &fallback->body : nullptr;
	}
};

struct InterfaceDeclaration : TypeDeclaration {
	Token                                   keyword;
	Token                                   kind;
	std::optional<TypeParameterList>        typeParameters;
	std::vector<TypeReference>              parents;
	std::vector<InterfaceMethodDeclaration> methods;

	InterfaceDeclaration (const Token &keyword, const Token &kind, const Token &name, std::optional<Token> visibility,
	                      std::optional<TypeParameterList> typeParameters, std::vector<TypeReference> parents,
	                      std::vector<InterfaceMethodDeclaration> methods, TokenSpan span, std::vector<TypeDeclarationPtr> nestedTypes = {}) :
	        TypeDeclaration(name, std::move(visibility), std::move(span), std::move(nestedTypes)),
	        keyword(keyword),
	        kind(kind),
	        typeParameters(std::move(typeParameters)),
	        parents(std::move(parents)),
	        methods(std::move(methods)) {
	}
};

}
