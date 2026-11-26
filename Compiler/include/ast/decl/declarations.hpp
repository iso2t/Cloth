// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct Declaration {
	TokenSpan span;

	explicit Declaration (TokenSpan span) : span(std::move(span)) {
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
	        Declaration{ std::move(span) },
	        name(name),
	        visibility(std::move(visibility)),
	        nestedTypes(std::move(nestedTypes)) {
	}

	~TypeDeclaration () override = default;
};

struct ParameterDeclaration {
	TypeReference        type;
	Token                name;
	TokenSpan            span;
	std::optional<Token> finalKeyword     = std::nullopt;
	std::optional<Token> optKeyword       = std::nullopt;
	std::optional<Token> defaultDelimiter = std::nullopt;
	ExpressionPtr        defaultValue     = nullptr;
	std::optional<Token> optOpening       = std::nullopt;
	std::optional<Token> optClosing       = std::nullopt;

	[[nodiscard]] bool isMutable () const {
		return !finalKeyword.has_value();
	}

	[[nodiscard]] bool isOptional () const {
		return defaultDelimiter.has_value();
	}

	[[nodiscard]] std::optional<TokenSpan> getOptionalTypeSpan () const {
		if (!optKeyword) {
			return std::nullopt;
		}
		return TokenSpan::spanThrough(optKeyword->getSpan(), optClosing ? optClosing->getSpan() : type.getSpan());
	}
};

/** Single-statement bodies carry an equals token. Brace bodies leave it empty. */
struct BlockBody {
	TokenSpan                 span;
	std::vector<StatementPtr> statements = {};
	std::optional<Token>      equals     = std::nullopt;
};

/** An equals-prefixed expression supplies an implicit return. */
struct ExpressionBody {
	Token         equals;
	ExpressionPtr expression;
	TokenSpan     span;
};

/** The compiler binding ID comes from a string literal in the standard library. */
struct IntrinsicBinding {
	Token operation;

	[[nodiscard]] std::string_view getId () const {
		const std::string_view s_name  = operation.getLexeme();
		const std::string_view s_quote = toString(ODoubleQuotation);
		if (s_name.size() >= 2 * s_quote.size() && s_name.starts_with(s_quote) && s_name.ends_with(s_quote)) {
			return s_name.substr(s_quote.size(), s_name.size() - 2 * s_quote.size());
		}
		return s_name;
	}
};

struct IntrinsicBody : IntrinsicBinding {
	Token     equals;
	TokenSpan span;

	IntrinsicBody (const Token &equals, const Token &operation) :
	        IntrinsicBinding{ operation },
	        equals(equals),
	        span(TokenSpan::spanThrough(equals.getSpan(), operation.getSpan())) {
	}
};

struct FunctionBody {
	std::variant<BlockBody, ExpressionBody, IntrinsicBody> value;

	[[nodiscard]] TokenSpan getSpan () const {
		return std::visit([]<typename Body>(const Body &body) -> TokenSpan { return body.span; }, value);
	}

	[[nodiscard]] const Token *getEquals () const {
		return std::visit(
		        []<typename Body>(const Body &body) -> const Token * {
			        if constexpr (std::is_same_v<Body, BlockBody>) {
				        return body.equals ? &*body.equals : nullptr;
			        } else {
				        return &body.equals;
			        }
		        },
		        value);
	}
};

/** A callable signature may stand on its own or have an executable body. */
struct CallableSignature : Declaration {
	Token                             keyword;
	std::optional<Token>              visibility;
	std::vector<TypeReference>        returnTypes;
	std::vector<ParameterDeclaration> parameters;

	~CallableSignature () override                       = default;
	[[nodiscard]] virtual const Token &getName () const = 0;

	[[nodiscard]] virtual const FunctionBody *executableBody () const {
		return nullptr;
	}

    protected:
	CallableSignature (const Token &keyword, std::vector<ParameterDeclaration> parameters, const TokenSpan &span,
	                   std::vector<TypeReference> returnTypes = {}, std::optional<Token> visibility = std::nullopt) :
	        Declaration{ span },
	        keyword(keyword),
	        visibility(std::move(visibility)),
	        returnTypes(std::move(returnTypes)),
	        parameters(std::move(parameters)) {
	}
};

struct ConstructorGuaranteeDeclaration : CallableSignature {
	ConstructorGuaranteeDeclaration (const Token &keyword, std::vector<ParameterDeclaration> parameters, const TokenSpan &span) :
	        CallableSignature(keyword, std::move(parameters), span) {
	}

	[[nodiscard]] const Token &getName () const override {
		return keyword;
	}
};

struct CallableDeclaration : CallableSignature {
	Token        name;
	FunctionBody body;

	[[nodiscard]] const Token &getName () const override {
		return name;
	}

	[[nodiscard]] const FunctionBody *executableBody () const override {
		return &body;
	}

    protected:
	CallableDeclaration (const Token &keyword, const Token &name, std::vector<ParameterDeclaration> parameters, FunctionBody body, const TokenSpan &start,
	                     std::vector<TypeReference> returnTypes = {}, const std::optional<Token> &visibility = std::nullopt) :
	        CallableSignature(keyword, std::move(parameters), TokenSpan::spanThrough(start, body.getSpan()), std::move(returnTypes), visibility),
	        name(name),
	        body(std::move(body)) {
	}
};

}
