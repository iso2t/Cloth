// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct ExpressionStatement : Statement {
	ExpressionPtr expression;

	explicit ExpressionStatement (ExpressionPtr expression) : expression(std::move(expression)) {
	}

	TokenSpan get_span () const override {
		return expression->get_span();
	}
};

struct BindingDeclaration : Statement {
	std::optional<TypeReference> type;
	Token                        name;
	ExpressionPtr                initializer;
	bool                         isMutable;
	TokenSpan                    span;

	BindingDeclaration (std::optional<TypeReference> type, const Token &name, ExpressionPtr initializer, bool isMutable,
	                    std::optional<TokenSpan> span = std::nullopt) :
	        type(std::move(type)),
	        name(name),
	        initializer(std::move(initializer)),
	        isMutable(isMutable),
	        span(span ? *span : TokenSpan::span_through(this->type ? this->type->get_span() : name.get_span(), this->initializer->get_span())) {
	}

	TokenSpan get_span () const override {
		return span;
	}
};

struct ReceivingTarget {
	struct Named {
		Token token;
	};

	struct Discard {
		Token token;
	};

	std::variant<Named, Discard> value;

	static ReceivingTarget of (const Token &token) {
		if (token.get_lexeme() == cloth::to_string(Operator::o_UNDERSCORE)) {
			return { Discard{ token } };
		}
		return { Named{ token } };
	}

	const Token &get_token () const {
		return std::visit([]<typename Target>(const Target &target) -> const Token & { return target.token; }, value);
	}

	TokenSpan get_span () const {
		return get_token().get_span();
	}
};

struct BindingGroupDeclaration : Statement {
	std::vector<TypeReference>   types;
	std::vector<ReceivingTarget> targets;
	ExpressionPtr                initializer;
	bool                         isMutable;
	TokenSpan                    span;

	BindingGroupDeclaration (std::vector<TypeReference> types, std::vector<ReceivingTarget> targets, ExpressionPtr initializer, bool isMutable,
	                         TokenSpan span) :
	        types(std::move(types)),
	        targets(std::move(targets)),
	        initializer(std::move(initializer)),
	        isMutable(isMutable),
	        span(span) {
	}

	TokenSpan get_span () const override {
		return span;
	}
};

struct AssignmentStatement : Statement {
	Token                name;
	ExpressionPtr        value;
	std::optional<Token> compoundOperator;

	AssignmentStatement (const Token &name, ExpressionPtr value, std::optional<Token> compoundOperator = std::nullopt) :
	        name(name),
	        value(std::move(value)),
	        compoundOperator(std::move(compoundOperator)) {
	}

	const BinaryExpression *get_compound () const {
		return compoundOperator ? &dynamic_cast<const BinaryExpression &>(*value) : nullptr;
	}

	TokenSpan get_span () const override {
		return TokenSpan::span_through(name.get_span(), value->get_span());
	}
};

struct MultiAssignmentStatement : Statement {
	std::vector<ReceivingTarget> targets;
	ExpressionPtr                value;

	MultiAssignmentStatement (std::vector<ReceivingTarget> targets, ExpressionPtr value) : targets(std::move(targets)), value(std::move(value)) {
	}

	std::vector<Token> get_names () const {
		std::vector<Token> v_names;
		v_names.reserve(targets.size());
		for (const ReceivingTarget &rt_target : targets) {
			v_names.push_back(rt_target.get_token());
		}
		return v_names;
	}

	TokenSpan get_span () const override {
		return TokenSpan::span_through(targets.at(0).get_span(), value->get_span());
	}
};

struct ElementAssignmentStatement : Statement {
	std::shared_ptr<const IndexExpression>  target;
	Token                                   op;
	ExpressionPtr                           value;
	std::shared_ptr<const BinaryExpression> compound;

	ElementAssignmentStatement (std::shared_ptr<const IndexExpression> target, const Token &op, ExpressionPtr value);

	TokenSpan get_span () const override {
		return TokenSpan::span_through(target->get_span(), value ? value->get_span() : op.get_span());
	}
};

struct FieldWriteStatement : Statement {
	ExpressionPtr                           target;
	Token                                   op;
	ExpressionPtr                           value;
	std::shared_ptr<const BinaryExpression> compound;

	FieldWriteStatement (ExpressionPtr target, const Token &op, ExpressionPtr value);

	TokenSpan get_span () const override {
		return TokenSpan::span_through(target->get_span(), value ? value->get_span() : op.get_span());
	}
};

struct UpdateStatement : Statement {
	Token                                 name;
	Token                                 op;
	std::shared_ptr<const NameExpression> target;

	UpdateStatement (const Token &name, const Token &op) : name(name), op(op), target(std::make_shared<NameExpression>(name)) {
	}

	TokenSpan get_span () const override {
		return TokenSpan::span_through(name.get_span(), op.get_span());
	}
};

std::pair<ExpressionPtr, ExpressionPtr> updateOperands (const Statement &statement);

struct ReturnStatement : Statement {
	Token         keyword;
	ExpressionPtr value;
	TokenSpan     span;

	ReturnStatement (const Token &keyword, ExpressionPtr value, std::optional<TokenSpan> span = std::nullopt) :
	        keyword(keyword),
	        value(std::move(value)),
	        span(span.value_or(keyword.get_span())) {
		if (!span && this->value) {
			this->span = TokenSpan::span_through(this->span, this->value->get_span());
		}
	}

	TokenSpan get_span () const override {
		return span;
	}
};

}
