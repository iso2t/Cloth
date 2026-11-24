// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/Bindings.rst

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/bindings_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {

bool BindingsParser::startsBinding () const {
	if (p_parser.atKeyword(KDef)) {
		return true;
	}
	int                i_start      = 0;
	std::optional<int> opt_typedEnd = p_parser.p_typeParser.typedTargetEnd();
	if (!opt_typedEnd) {
		return false;
	}
	while (true) {
		const int i_end = opt_typedEnd.value_or(i_start + 1);
		// Only a named type with '>' immediately before the target can also be a comparison.
		if (opt_typedEnd && (p_parser.peek(i_start).getType() != TokenType::Identifier || !p_parser.at(OGreater, i_end - 2))) {
			return true;
		}
		if (!p_parser.at(OComma, i_end)) {
			return p_parser.at(OAssign, i_end) || p_parser.at(OFinalAssign, i_end);
		}
		i_start      = p_parser.afterNewLines(i_end + 1);
		opt_typedEnd = p_parser.p_typeParser.typedTargetEnd(i_start);
		if (!opt_typedEnd && !Parser::isReceivingTarget(p_parser.peek(i_start))) {
			return false;
		}
	}
}

std::optional<TypeReference> BindingsParser::parseBindingType () const {
	if (!p_parser.atKeyword(KDef)) {
		return p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
	}
	p_parser.advance();
	if (p_parser.at(OLess)) {
		throw cde::SyntaxErrors::expected(std::format("a name after '{}' or an explicit type before the name", toString(KDef)), p_parser.peek());
	}
	return std::nullopt;
}

std::vector<StatementPtr> BindingsParser::parseBindings (bool allowWhileTrailer) const {
	const Token                t_start = p_parser.peek();
	std::vector<TypeReference> v_types;
	if (std::optional<TypeReference> opt_first = parseBindingType()) {
		v_types.push_back(std::move(*opt_first));
	}
	std::vector v_targets{ p_parser.parseReceivingTarget() };
	while (p_parser.at(OComma)) {
		p_parser.advance();
		if (!v_types.empty()) {
			p_parser.skipNewLines();
			if (p_parser.p_typeParser.typedTargetEnd()) {
				v_types.push_back(p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
			}
		}
		v_targets.push_back(p_parser.parseReceivingTarget());
	}

	const Token        t_assignment = p_parser.peek();
	const std::string &s_assignment = t_assignment.getLexeme();
	const bool         b_isMutable  = s_assignment == toString(OAssign);
	if (t_assignment.getType() != TokenType::Operator || (!b_isMutable && s_assignment != toString(OFinalAssign))) {
		throw cde::SyntaxErrors::expected(std::format("'{}' or '{}'", toString(OAssign), toString(OFinalAssign)), t_assignment);
	}
	p_parser.advance();

	const int                    i_initializerStart = p_parser.i_currentIndex;
	std::optional<TypeReference> opt_type           = v_types.size() == 1 ? std::make_optional(v_types.front()) : std::nullopt;
	const bool                   b_namedValue       = !v_types.empty() && v_types.front().getName().getType() == TokenType::Identifier &&
	                          v_types.front().getArraySuffixes().empty();
	const std::function fn_braces = [this, &t_start, &v_types, b_namedValue] (const Token &name) -> ExpressionPtr {
		return ExpressionPtr(p_parser.parseBindingBrace(b_namedValue || v_types.empty(), b_namedValue,
		                                                [&t_start, &v_types, &name] (BlockBody body) -> std::unique_ptr<FunctionBlockExpression> {
			                                                return std::make_unique<FunctionBlockExpression>(FunctionDeclaration(
			                                                        t_start, name, v_types, {}, FunctionBody{ std::move(body) }));
		                                                }));
	};

	ExpressionPtr ep_braced;
	if (p_parser.peek().getLexeme() == toString(OBraceOpen)) {
		ep_braced = fn_braces(v_targets.front().getToken());
	}
	const bool b_functionBlock = dynamic_cast<const FunctionBlockExpression *>(ep_braced.get()) != nullptr;
	if (b_functionBlock && v_types.size() <= 1 &&
	    std::ranges::all_of(v_targets, [] (const ReceivingTarget &target) { return std::holds_alternative<ReceivingTarget::Named>(target.value); })) {
		std::vector<StatementPtr> v_bindings;
		v_bindings.reserve(v_targets.size());
		for (std::size_t st_index = 0; st_index < v_targets.size(); ++st_index) {
			const Token  &t_name         = v_targets[st_index].getToken();
			ExpressionPtr ep_initializer = ep_braced;
			if (st_index != 0) {
				// Each deferred binding has its own function body and AST identity.
				p_parser.i_currentIndex = i_initializerStart;
				ep_initializer          = fn_braces(t_name);
			}
			const TokenSpan ts_span = TokenSpan::spanThrough(t_start.getSpan(), ep_initializer->getSpan());
			v_bindings.push_back(std::make_shared<BindingDeclaration>(opt_type, t_name, std::move(ep_initializer), b_isMutable, ts_span));
		}
		return v_bindings;
	}

	ExpressionPtr ep_initializer;
	if (b_functionBlock) {
		ep_initializer = std::move(ep_braced);
	} else if (!ep_braced) {
		ep_initializer = p_parser.parseResults(nullptr, p_parser.peek().getSpan(), allowWhileTrailer, /*multiline=*/false);
	} else {
		const TokenSpan ts_bracedStart = ep_braced->getSpan();
		ExpressionPtr   ep_primary     = p_parser.p_expressionParser.continueFromPrimary(std::move(ep_braced), allowWhileTrailer);
		ep_initializer                 = p_parser.parseResults(std::move(ep_primary), ts_bracedStart, allowWhileTrailer, /*multiline=*/false);
	}

	const TokenSpan ts_span = TokenSpan::spanThrough(t_start.getSpan(), p_parser.previousStatementEnd());
	if (v_targets.size() == 1 && std::holds_alternative<ReceivingTarget::Named>(v_targets.front().value) && v_types.size() <= 1) {
		return { std::make_shared<BindingDeclaration>(std::move(opt_type), v_targets.front().getToken(), std::move(ep_initializer), b_isMutable,
			                                      ts_span) };
	}
	return { std::make_shared<BindingGroupDeclaration>(std::move(v_types), std::move(v_targets), std::move(ep_initializer), b_isMutable, ts_span) };
}

}
