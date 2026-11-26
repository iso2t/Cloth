// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ast/decl/function_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/type/type_reference.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth {

// Visit expression operands and conditional branch scopes in source order. The caller resolves value-dependent type-test targets and deferred bodies.
void visitOperands (const Expression &node, const std::function<void(const Expression &)> &visit,
                    const std::function<void(const std::vector<StatementPtr> &)> &visitBlock);

struct NameExpression : Expression {
	Token name;

	explicit NameExpression (const Token &name);

	[[nodiscard]] TokenSpan getSpan () const override {
		return name.getSpan();
	}
};

struct FunctionBlockExpression : Expression {
	FunctionDeclaration declaration;

	explicit FunctionBlockExpression (FunctionDeclaration declaration);

	[[nodiscard]] TokenSpan getSpan () const override {
		return declaration.body.getSpan();
	}
};

struct CallArgument {
	ExpressionPtr        expression;
	std::optional<Token> label  = std::nullopt;
	std::optional<Token> equals = std::nullopt;

	[[nodiscard]] TokenSpan getSpan () const;
};

struct CallSite : Expression {
	Token                     name;
	std::vector<CallArgument> suppliedArguments;

	[[nodiscard]] std::vector<ExpressionPtr> getArguments () const;

    protected:
	CallSite (const Token &name, std::vector<CallArgument> suppliedArguments);
};

struct CallExpression : CallSite {
	TokenSpan                       span;
	std::optional<TypeArgumentList> typeArguments;

	CallExpression (const Token &name, std::vector<CallArgument> suppliedArguments, TokenSpan span,
	                std::optional<TypeArgumentList> typeArguments = std::nullopt);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct ArrayLiteral : Expression {
	std::vector<ExpressionPtr> elements;
	TokenSpan                  span;

	ArrayLiteral (std::vector<ExpressionPtr> elements, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct ResultListExpression : Expression {
	std::vector<ExpressionPtr> values;
	TokenSpan                  span;

	ResultListExpression (std::vector<ExpressionPtr> values, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct IndexExpression : Expression {
	ExpressionPtr array;
	ExpressionPtr index;
	TokenSpan     span;

	IndexExpression (ExpressionPtr array, ExpressionPtr index, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct UnaryExpression : Expression {
	Token         op;
	ExpressionPtr operand;

	UnaryExpression (const Token &op, ExpressionPtr operand);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(op.getSpan(), operand->getSpan());
	}
};

struct BinaryExpression : Expression {
	ExpressionPtr left;
	Token         op;
	ExpressionPtr right;

	BinaryExpression (ExpressionPtr left, const Token &op, ExpressionPtr right);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(left->getSpan(), right->getSpan());
	}
};

std::shared_ptr<const BinaryExpression> compoundExpression (const ExpressionPtr &target, const Token &op, const ExpressionPtr &value);

const BinaryExpression *rangeOrNull (const Expression &expression);

struct ElvisExpression : Expression {
	ExpressionPtr left;
	Token         op;
	ExpressionPtr fallback;

	ElvisExpression (ExpressionPtr left, const Token &op, ExpressionPtr fallback);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(left->getSpan(), fallback->getSpan());
	}
};

struct GuardedReceiver : Expression {
	ExpressionPtr source;

	explicit GuardedReceiver (ExpressionPtr source);

	[[nodiscard]] TokenSpan getSpan () const override {
		return source->getSpan();
	}
};

/** The continuation can refer to the same payload after its source is evaluated once. */
struct GuardedAccessExpression : Expression {
	Token                                  op;
	std::shared_ptr<const GuardedReceiver> payload;
	ExpressionPtr                          continuation;

	GuardedAccessExpression (const Token &op, std::shared_ptr<const GuardedReceiver> payload, ExpressionPtr continuation);

	[[nodiscard]] const ExpressionPtr &getReceiver () const {
		return payload->source;
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(getReceiver()->getSpan(), continuation->getSpan());
	}
};

struct NullLiteral : Expression {
	TokenSpan span;

	explicit NullLiteral (TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct CastExpression : Expression {
	ExpressionPtr operand;
	Token         op;
	TypeReference target;

	CastExpression (ExpressionPtr operand, const Token &op, TypeReference target);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(operand->getSpan(), target.getSpan());
	}
};

struct TypeCheckExpression : Expression {
	ExpressionPtr                                operand;
	Token                                        op;
	TypeReference                                target;
	std::shared_ptr<const FieldAccessExpression> valueCandidate;

	TypeCheckExpression (ExpressionPtr operand, const Token &op, TypeReference target,
	                     std::shared_ptr<const FieldAccessExpression> valueCandidate = nullptr);

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(operand->getSpan(), target.getSpan());
	}
};

struct ConditionalBranch {
	ExpressionPtr             condition;
	std::vector<StatementPtr> statements;
	TokenSpan                 span;
};

struct IfExpression : Expression {
	Token                                    keyword;
	std::vector<ConditionalBranch>           branches;
	std::optional<std::vector<StatementPtr>> otherwise;
	TokenSpan                                span;
	std::optional<TokenSpan>                 otherwiseSpan;

	IfExpression (const Token &keyword, std::vector<ConditionalBranch> branches, std::optional<std::vector<StatementPtr>> otherwise, TokenSpan span,
	              std::optional<TokenSpan> otherwiseSpan);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct NumberLiteral : Expression {};

struct IntegerLiteral : NumberLiteral {
	IntegerValue value;
	Token        token;

	IntegerLiteral (IntegerValue value, const Token &token);

	[[nodiscard]] TokenSpan getSpan () const override {
		return token.getSpan();
	}
};

/** Preserve the literal's text until emission selects a floating-point format. */
struct FloatingPointLiteral : NumberLiteral {
	std::string value;
	Token       token;

	FloatingPointLiteral (std::string value, const Token &token);

	[[nodiscard]] TokenSpan getSpan () const override {
		return token.getSpan();
	}
};

struct BooleanLiteral : Expression {
	bool      value;
	TokenSpan span;

	BooleanLiteral (bool value, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct CharacterLiteral : Expression {
	char32_t  value;
	TokenSpan span;

	CharacterLiteral (char32_t value, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct StringLiteral : Expression {
	std::string value;
	TokenSpan   span;

	StringLiteral (std::string value, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

struct InterpolatedString : Expression {
	// Even positions are literal text segments (possibly empty). Odd positions are expressions in braces.
	std::vector<ExpressionPtr> parts;
	TokenSpan                  span;

	InterpolatedString (std::vector<ExpressionPtr> parts, TokenSpan span);

	[[nodiscard]] TokenSpan getSpan () const override {
		return span;
	}
};

}
