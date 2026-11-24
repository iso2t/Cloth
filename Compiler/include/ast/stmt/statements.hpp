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

	[[nodiscard]] TokenSpan getSpan () const override {
		return expression->getSpan();
	}
};

struct BindingDeclaration : Statement {
	std::optional<TypeReference> type;
	Token                        name;
	ExpressionPtr                initializer;
	bool                         isMutable;
	TokenSpan                    span;

	BindingDeclaration (std::optional<TypeReference> type, const Token &name, ExpressionPtr initializer, const bool isMutable,
	                    const std::optional<TokenSpan> &span = std::nullopt) :
	        type(std::move(type)),
	        name(name),
	        initializer(std::move(initializer)),
	        isMutable(isMutable),
	        span(span ? *span : TokenSpan::spanThrough(this->type ? this->type->getSpan() : name.getSpan(), this->initializer->getSpan())) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
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
		if (token.getLexeme() == toString(OUnderscore)) {
			return { Discard{ token } };
		}
		return { Named{ token } };
	}

	[[nodiscard]] const Token &getToken () const {
		return std::visit([]<typename Target>(const Target &target) -> const Token & { return target.token; }, value);
	}

	[[nodiscard]] TokenSpan getSpan () const {
		return getToken().getSpan();
	}
};

struct BindingGroupDeclaration : Statement {
	std::vector<TypeReference>   types;
	std::vector<ReceivingTarget> targets;
	ExpressionPtr                initializer;
	bool                         isMutable;
	TokenSpan                    span;

	BindingGroupDeclaration (std::vector<TypeReference> types, std::vector<ReceivingTarget> targets, ExpressionPtr initializer, const bool isMutable,
	                         TokenSpan span) :
	        types(std::move(types)),
	        targets(std::move(targets)),
	        initializer(std::move(initializer)),
	        isMutable(isMutable),
	        span(std::move(span)) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
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

	[[nodiscard]] const BinaryExpression *getCompound () const {
		return compoundOperator ? &dynamic_cast<const BinaryExpression &>(*value) : nullptr;
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(name.getSpan(), value->getSpan());
	}
};

struct MultiAssignmentStatement : Statement {
	std::vector<ReceivingTarget> targets;
	ExpressionPtr                value;

	MultiAssignmentStatement (std::vector<ReceivingTarget> targets, ExpressionPtr value) : targets(std::move(targets)), value(std::move(value)) {
	}

	[[nodiscard]] std::vector<Token> getNames () const {
		std::vector<Token> v_names;
		v_names.reserve(targets.size());
		for (const ReceivingTarget &rt_target : targets) {
			v_names.push_back(rt_target.getToken());
		}
		return v_names;
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(targets.at(0).getSpan(), value->getSpan());
	}
};

struct ElementAssignmentStatement : Statement {
	std::shared_ptr<const IndexExpression>  target;
	Token                                   op;
	ExpressionPtr                           value;
	std::shared_ptr<const BinaryExpression> compound;

	ElementAssignmentStatement (std::shared_ptr<const IndexExpression> target, const Token &op, ExpressionPtr value);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(target->getSpan(), value ? value->getSpan() : op.getSpan());
	}
};

struct FieldWriteStatement : Statement {
	ExpressionPtr                           target;
	Token                                   op;
	ExpressionPtr                           value;
	std::shared_ptr<const BinaryExpression> compound;

	FieldWriteStatement (ExpressionPtr target, const Token &op, ExpressionPtr value);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(target->getSpan(), value ? value->getSpan() : op.getSpan());
	}
};

struct UpdateStatement : Statement {
	Token                                 name;
	Token                                 op;
	std::shared_ptr<const NameExpression> target;

	UpdateStatement (const Token &name, const Token &op) : name(name), op(op), target(std::make_shared<NameExpression>(name)) {
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(name.getSpan(), op.getSpan());
	}
};

std::pair<ExpressionPtr, ExpressionPtr> updateOperands (const Statement &statement);

struct ReturnStatement : Statement {
	Token         keyword;
	ExpressionPtr value;
	TokenSpan     span;

	ReturnStatement (const Token &keyword, ExpressionPtr value, const std::optional<TokenSpan> &span = std::nullopt) :
	        keyword(keyword),
	        value(std::move(value)),
	        span(span.value_or(keyword.getSpan())) {
		if (!span && this->value) {
			this->span = TokenSpan::spanThrough(this->span, this->value->getSpan());
		}
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

}
