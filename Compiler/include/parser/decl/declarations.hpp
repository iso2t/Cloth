// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <token/token.hpp>

namespace cloth {

struct Declaration {
	TokenSpan span;

	explicit Declaration (TokenSpan span) : span(span) {
	}

	virtual ~Declaration () = default;
};

using DeclarationPtr = std::shared_ptr<const Declaration>;

struct TypeDeclaration;
using TypeDeclarationPtr = std::shared_ptr<const TypeDeclaration>;

struct TypeDeclaration : Declaration {
	Token                           name;
	std::optional<Token>            visibility;
	std::vector<TypeDeclarationPtr> nestedTypes;

	TypeDeclaration (const Token &name, std::optional<Token> visibility, TokenSpan span, std::vector<TypeDeclarationPtr> nestedTypes = {}) :
	        Declaration{ span },
	        name(name),
	        visibility(std::move(visibility)),
	        nestedTypes(std::move(nestedTypes)) {
	}

	virtual ~TypeDeclaration () = default;
};

struct ParameterDeclaration {
	Token                name;
	TokenSpan            span;
	std::optional<Token> finalKeyword     = std::nullopt;
	std::optional<Token> optKeyword       = std::nullopt;
	std::optional<Token> defaultDelimiter = std::nullopt;
	std::optional<Token> optOpening       = std::nullopt;
	std::optional<Token> optClosing       = std::nullopt;

	bool isMutable () const {
		return !finalKeyword.has_value();
	}

	bool isOptional () const {
		return defaultDelimiter.has_value();
	}
};

/** Single-statement bodies carry an equals token. Brace bodies leave it empty. */
struct BlockBody {
	TokenSpan            span;
	std::optional<Token> equals = std::nullopt;
};

/** An equals-prefixed expression supplies an implicit return. */
struct ExpressionBody {
	Token     equals;
	TokenSpan span;
};

struct FunctionBody {
	std::variant<BlockBody, ExpressionBody> value;

	TokenSpan get_span () const {
		return std::visit([]<typename Body>(const Body &body) -> TokenSpan { return body.span; }, value);
	}

	const Token *get_equals () const {
		if (const BlockBody *bb_block = std::get_if<BlockBody>(&value)) {
			return bb_block->equals ? &*bb_block->equals : nullptr;
		}
		return &std::get<ExpressionBody>(value).equals;
	}
};

/** A callable signature may stand on its own or have an executable body. */
struct CallableSignature : Declaration {
	Token                             keyword;
	std::vector<ParameterDeclaration> parameters;

	virtual ~CallableSignature ()          = default;
	virtual const Token &get_name () const = 0;

	virtual const FunctionBody *executableBody () const {
		return nullptr;
	}

    protected:
	CallableSignature (const Token &keyword, std::vector<ParameterDeclaration> parameters, TokenSpan span) :
	        Declaration{ span },
	        keyword(keyword),
	        parameters(std::move(parameters)) {
	}
};

struct ConstructorGuaranteeDeclaration : CallableSignature {
	ConstructorGuaranteeDeclaration (const Token &keyword, std::vector<ParameterDeclaration> parameters, TokenSpan span) :
	        CallableSignature(keyword, std::move(parameters), span) {
	}

	const Token &get_name () const override {
		return keyword;
	}
};

struct CallableDeclaration : CallableSignature {
	Token        name;
	FunctionBody body;

	const Token &get_name () const override {
		return name;
	}

	const FunctionBody *executableBody () const override {
		return &body;
	}

    protected:
	CallableDeclaration (const Token &keyword, const Token &name, std::vector<ParameterDeclaration> parameters, FunctionBody body, TokenSpan start) :
	        CallableSignature(keyword, std::move(parameters), spanThrough(start, body.get_span())),
	        name(name),
	        body(std::move(body)) {
	}

    private:
	static TokenSpan spanThrough (TokenSpan start, TokenSpan end) {
		return { start.startLine, start.startCol, end.endLine, end.endCol };
	}
};

struct FunctionDeclaration : CallableDeclaration {
	std::optional<Token> visibility;

	FunctionDeclaration (const Token &keyword, const Token &name, std::vector<ParameterDeclaration> parameters, FunctionBody body,
	                     std::optional<Token> visibility = std::nullopt) :
	        CallableDeclaration(keyword, name, std::move(parameters), std::move(body), (visibility ? *visibility : keyword).get_span()),
	        visibility(std::move(visibility)) {
	}
};

using Declarations = std::variant<TypeDeclarationPtr, ConstructorGuaranteeDeclaration, FunctionDeclaration>;
}
