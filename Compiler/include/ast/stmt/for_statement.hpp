// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include <ast/expr/expression.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

struct ForBinding {
	Token                        keyword;
	std::optional<TypeReference> type;
	Token                        name;
	std::optional<TokenSpan>     annotationSpan;

	[[nodiscard]] TokenSpan get_span () const {
		return TokenSpan::span_through(keyword.get_span(), name.get_span());
	}
};

struct ForSource {
	struct Range {
		ExpressionPtr start;
		Token         delimiter;
		ExpressionPtr end;
		TokenSpan     span;

		[[nodiscard]] bool isInclusive () const {
			return delimiter.get_lexeme() == to_string(oRangeInclusive);
		}
	};

	struct Elements {
		ExpressionPtr expression;
		TokenSpan     span;
	};

	std::variant<Range, Elements> value;

	[[nodiscard]] TokenSpan get_span () const {
		return std::visit([]<typename Source>(const Source &source) -> TokenSpan { return source.span; }, value);
	}
};

struct ForStepper {
	Token         target;
	Token         op;
	ExpressionPtr value;
	TokenSpan     span;
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

	[[nodiscard]] const LoopTargetPtr &get_loop_target () const override {
		return loopTarget;
	}

	[[nodiscard]] TokenSpan get_header_span () const {
		return TokenSpan::span_through(keyword.get_span(), closing.get_span());
	}

	[[nodiscard]] TokenSpan get_span () const override {
		return TokenSpan::span_through(keyword.get_span(), body.span);
	}
};

}
