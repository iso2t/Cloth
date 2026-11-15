// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <token/token.hpp>

namespace cloth {

struct Declaration {
	TokenSpan span;
};

struct TypeDeclaration : Declaration {
	Token token;
	std::optional<Token> visibility;
	std::optional<TypeParameterList> typeParameters;
	std::vector<TypeDeclaration> nestedTypes;
};

struct CallableSignature : Declaration {
	Token keyword;
	Token name;
	std::vector<TypeReference> returnTypes;
	std::vector<ParameterDeclaration> parameters;
};

struct CallableDeclaration : CallableSignature {
	FunctionBody body;
};

// internal val CallableSignature.executableBody: FunctionBody?
// 	get() = when (this) {
// 		is CallableDeclaration             -> body
// 		is InterfaceMethodDeclaration      -> fallback?.body
// 		is ConstructorGuaranteeDeclaration -> null
// 		is StateMethodContractDeclaration  -> null
// 		is StateActivationDeclaration      -> initializer.body
// 		is StateInitialPayloadDeclaration  -> null
// 	}

struct ConstructorGuaranteeDeclaraton: CallableSignature {
	Token keyword;
	std::vector<ParameterDeclaration> parameters;
	TokenSpan span;

	std::string get_name() {
		return keyword.to_string();
	}

	std::vector<TypeReference> get() {
		return NULL;
	}
};

struct FunctionDeclaration: CallableDeclaration {
	
};

struct ParameterDeclaration {
	
};

struct FunctionBody {
	TokenSpan span;
	std::optional<Token> equals;
};

struct BlockBody: FunctionBody {
	
};

struct ExpressionBody: FunctionBody {
	
};

struct IntrinsicBody: FunctionBody {
	
};

using Declarations = std::variant<TypeDeclaration, CallableSignature, CallableDeclaration, ConstructorGuaranteeDeclaraton, ParameterDeclaration, FunctionBody>;
}
