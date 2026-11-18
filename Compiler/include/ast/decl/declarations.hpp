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

	[[nodiscard]] std::optional<TokenSpan> get_optional_type_span () const {
		if (!optKeyword) {
			return std::nullopt;
		}
		return TokenSpan::span_through(optKeyword->get_span(), optClosing ? optClosing->get_span() : type.get_span());
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

/** The operation name comes from a string literal in a library binding. */
struct IntrinsicBody {
	Token     equals;
	Token     operation;
	TokenSpan span;

	IntrinsicBody (const Token &equals, const Token &operation) :
	        equals(equals),
	        operation(operation),
	        span(TokenSpan::span_through(equals.get_span(), operation.get_span())) {
	}

	[[nodiscard]] std::string_view get_id () const {
		const std::string_view s_name = operation.get_lexeme();
		const std::string_view s_quote = to_string(oDoubleQuotation);
		if (s_name.size() >= 2 * s_quote.size() && s_name.starts_with(s_quote) && s_name.ends_with(s_quote)) {
			return s_name.substr(s_quote.size(), s_name.size() - 2 * s_quote.size());
		}
		return s_name;
	}
};

struct FunctionBody {
	std::variant<BlockBody, ExpressionBody, IntrinsicBody> value;

	[[nodiscard]] TokenSpan get_span () const {
		return std::visit([]<typename Body>(const Body &body) -> TokenSpan { return body.span; }, value);
	}

	[[nodiscard]] const Token *get_equals () const {
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
	[[nodiscard]] virtual const Token &get_name () const = 0;

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

	[[nodiscard]] const Token &get_name () const override {
		return keyword;
	}
};

struct CallableDeclaration : CallableSignature {
	Token        name;
	FunctionBody body;

	[[nodiscard]] const Token &get_name () const override {
		return name;
	}

	[[nodiscard]] const FunctionBody *executableBody () const override {
		return &body;
	}

    protected:
	CallableDeclaration (const Token &keyword, const Token &name, std::vector<ParameterDeclaration> parameters, FunctionBody body, const TokenSpan &start,
	                     std::vector<TypeReference> returnTypes = {}, const std::optional<Token> &visibility = std::nullopt) :
	        CallableSignature(keyword, std::move(parameters), TokenSpan::span_through(start, body.get_span()), std::move(returnTypes), visibility),
	        name(name),
	        body(std::move(body)) {
	}
};

}
