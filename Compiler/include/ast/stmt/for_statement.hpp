// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include <ast/expr/expressions.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct ForBinding {
	Token                        start;
	std::optional<TypeReference> type;
	Token                        name;

	[[nodiscard]] TokenSpan getSpan () const {
		return TokenSpan::spanThrough(start.getSpan(), name.getSpan());
	}
};

struct ForSource {
	struct Range {
		std::shared_ptr<const BinaryExpression> expression;
		TokenSpan                               span;

		[[nodiscard]] bool isInclusive () const {
			return expression->op.getOperator() == ORangeInclusive;
		}
	};

	struct Elements {
		ExpressionPtr expression;
		TokenSpan     span;
	};

	std::variant<Range, Elements> value;

	[[nodiscard]] const Expression &getExpression () const {
		return std::visit([]<typename Source>(const Source &source) -> const Expression & { return *source.expression; }, value);
	}

	[[nodiscard]] TokenSpan getSpan () const {
		return std::visit([]<typename Source>(const Source &source) -> TokenSpan { return source.span; }, value);
	}
};

struct ForStepper {
	Token                                   target;
	Token                                   op;
	ExpressionPtr                           value;
	TokenSpan                               span;
	std::shared_ptr<const BinaryExpression> compound = compoundExpression(std::make_shared<NameExpression>(target), op, value);

	[[nodiscard]] const Expression *update () const {
		return compound ? compound.get() : value.get();
	}
};

using ForBody = LoopBody;

struct ForStatement : LoopStatement {
	Token                     keyword;
	Token                     opening;
	ForBinding                binding;
	Token                     arrow;
	ForSource                 source;
	std::optional<Token>      separator;
	std::optional<ForStepper> stepper;
	Token                     closing;
	ForBody                   body;
	LoopTargetPtr             loopTarget = std::make_shared<LoopTarget>();

	ForStatement (const Token &keyword, const Token &opening, ForBinding binding, const Token &arrow, ForSource source, std::optional<Token> separator,
	              std::optional<ForStepper> stepper, const Token &closing, ForBody body) :
	        keyword(keyword),
	        opening(opening),
	        binding(std::move(binding)),
	        arrow(arrow),
	        source(std::move(source)),
	        separator(std::move(separator)),
	        stepper(std::move(stepper)),
	        closing(closing),
	        body(std::move(body)) {
	}

	[[nodiscard]] const LoopTargetPtr &getLoopTarget () const override {
		return loopTarget;
	}

	[[nodiscard]] TokenSpan getHeaderSpan () const {
		return TokenSpan::spanThrough(keyword.getSpan(), closing.getSpan());
	}

	[[nodiscard]] TokenSpan getSpan () const override {
		return TokenSpan::spanThrough(keyword.getSpan(), body.span);
	}
};

}
