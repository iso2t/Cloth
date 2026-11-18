// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct FunctionDeclaration : CallableDeclaration {
	std::optional<Token>             visibility;
	std::optional<TypeParameterList> typeParameters;

	FunctionDeclaration (const Token &keyword, const Token &name, std::vector<TypeReference> returnTypes, std::vector<ParameterDeclaration> parameters,
	                     FunctionBody body, std::optional<Token> visibility = std::nullopt,
	                     std::optional<TypeParameterList> typeParameters = std::nullopt) :
	        CallableDeclaration(keyword, name, std::move(parameters), std::move(body), (visibility ? *visibility : keyword).get_span(),
	                            std::move(returnTypes)),
	        visibility(std::move(visibility)),
	        typeParameters(std::move(typeParameters)) {
	}

	const TypeReference *get_return_type () const {
		return returnTypes.size() == 1 ? &returnTypes.front() : nullptr;
	}
};

using Declarations = std::variant<TypeDeclarationPtr, ConstructorGuaranteeDeclaration, FunctionDeclaration>;

}
