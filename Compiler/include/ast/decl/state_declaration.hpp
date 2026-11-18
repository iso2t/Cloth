// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/expr/expression.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct StateClassMember {
	virtual ~StateClassMember ()              = default;
	virtual const Token &get_keyword () const = 0;
	virtual TokenSpan    get_span () const    = 0;
};

using StateClassMemberPtr = std::shared_ptr<const StateClassMember>;

struct InitialStatePrefix {
	Token         keyword;
	ExpressionPtr payload;
	TokenSpan     span;
};

struct StateSelectionDeclaration : StateClassMember {
	std::optional<InitialStatePrefix> initial;

	virtual const Token &get_name () const = 0;

    protected:
	explicit StateSelectionDeclaration (std::optional<InitialStatePrefix> initial);
};

struct StateMember {
	virtual ~StateMember ()             = default;
	virtual TokenSpan get_span () const = 0;
};

using StateMemberPtr = std::shared_ptr<const StateMember>;

struct StateBodyDeclaration {
	Token                       keyword;
	std::optional<Token>        visibility;
	Token                       name;
	TypeReference               payloadType;
	std::vector<StateMemberPtr> members;

	virtual ~StateBodyDeclaration ()    = default;
	virtual TokenSpan get_span () const = 0;

    protected:
	StateBodyDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, TypeReference payloadType,
	                      std::vector<StateMemberPtr> members);
};

struct NestedStateDeclaration : StateSelectionDeclaration, StateBodyDeclaration {
	TokenSpan span;

	NestedStateDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, TypeReference payloadType,
	                        std::optional<InitialStatePrefix> initial, std::vector<StateMemberPtr> members, TokenSpan span);

	const Token &get_keyword () const override {
		return keyword;
	}

	const Token &get_name () const override {
		return name;
	}

	TokenSpan get_span () const override {
		return span;
	}
};

struct StateAttachmentDeclaration : StateSelectionDeclaration {
	Token     keyword;
	Token     name;
	TokenSpan span;

	StateAttachmentDeclaration (const Token &keyword, const Token &name, std::optional<InitialStatePrefix> initial, TokenSpan span);

	const Token &get_keyword () const override {
		return keyword;
	}

	const Token &get_name () const override {
		return name;
	}

	TokenSpan get_span () const override {
		return span;
	}
};

struct TopLevelStateDeclaration : Declaration, StateBodyDeclaration {
	TypeReference                   host;
	std::vector<TypeDeclarationPtr> nestedTypes;

	TopLevelStateDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, TypeReference payloadType, TypeReference host,
	                          std::vector<StateMemberPtr> members, TokenSpan span, std::vector<TypeDeclarationPtr> nestedTypes = {});

	TokenSpan get_span () const override {
		return span;
	}
};

struct StateMethodContractDeclaration : StateClassMember, CallableSignature {
	std::optional<Token>             visibility;
	Token                            name;
	std::optional<TypeParameterList> typeParameters;

	StateMethodContractDeclaration (const Token &keyword, std::optional<Token> visibility, const Token &name, std::vector<TypeReference> returnTypes,
	                                std::vector<ParameterDeclaration> parameters, TokenSpan span,
	                                std::optional<TypeParameterList> typeParameters = std::nullopt);

	const Token &get_keyword () const override {
		return keyword;
	}

	const Token &get_name () const override {
		return name;
	}

	TokenSpan get_span () const override {
		return span;
	}
};

struct StateInitializerDeclaration : StateMember {
	Token        keyword;
	FunctionBody body;

	StateInitializerDeclaration (const Token &keyword, BlockBody body);

	TokenSpan get_span () const override {
		return TokenSpan::span_through(keyword.get_span(), body.get_span());
	}
};

struct StateActivationDeclaration : CallableSignature {
	Token                       name;
	StateInitializerDeclaration initializer;

	StateActivationDeclaration (const Token &name, StateInitializerDeclaration initializer);

	const Token &get_name () const override {
		return name;
	}

	std::vector<TypeReference> get_return_types () const {
		return {};
	}

	const FunctionBody *executableBody () const override {
		return &initializer.body;
	}
};

struct StateInitialPayloadDeclaration : CallableSignature {
	Token              name;
	InitialStatePrefix prefix;

	StateInitialPayloadDeclaration (const Token &name, InitialStatePrefix prefix);

	const Token &get_name () const override {
		return name;
	}

	std::vector<TypeReference> get_return_types () const {
		return {};
	}

	const ExpressionPtr &get_expression () const;
};

}
