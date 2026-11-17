// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

#include <parser/decl/declarations.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/parser.hpp>
#include <parser/stmt/statement.hpp>
#include <parser/stmt/statements.hpp>
#include <parser/subs/bindings_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/type/type_reference.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

std::vector<StatementPtr> BindingsParser::parseBindings (bool allowWhileTrailer) {
	const Token                  t_keyword = p_parser.advance();
	std::vector<TypeReference>   v_types   = p_parser.parseTypeList();
	std::vector<ReceivingTarget> v_targets{ p_parser.parseReceivingTarget() };
	while (p_parser.peek().get_lexeme() == cloth::to_string(Operator::o_COMMA)) {
		p_parser.advance();
		v_targets.push_back(p_parser.parseReceivingTarget());
	}

	const Token        t_assignment = p_parser.peek();
	const std::string &s_assignment = t_assignment.get_lexeme();
	const bool         b_isMutable  = s_assignment == cloth::to_string(Operator::o_ASSIGN);
	if (t_assignment.get_type() != TokenType::Operator || (!b_isMutable && s_assignment != cloth::to_string(Operator::o_FINAL_ASSIGN))) {
		throw cde::SyntaxErrors::expected(std::format("'{}' or '{}'", cloth::to_string(Operator::o_ASSIGN), cloth::to_string(Operator::o_FINAL_ASSIGN)),
		                             t_assignment);
	}
	p_parser.advance();

	const int                    i_initializerStart = p_parser.i_currentIndex;
	std::optional<TypeReference> opt_type           = v_types.size() == 1 ? std::make_optional(v_types.front()) : std::nullopt;
	const bool                   b_namedValue       = !v_types.empty() && v_types.front().get_name().get_type() == TokenType::Identifier &&
	                          v_types.front().get_array_suffixes().empty();
	const std::function<ExpressionPtr(const Token &)> fn_braces = [this, &t_keyword, &v_types, b_namedValue] (const Token &name) -> ExpressionPtr {
		return ExpressionPtr(p_parser.parseBindingBrace(b_namedValue || v_types.empty(), b_namedValue,
		                                                [&t_keyword, &v_types, &name] (BlockBody body) -> std::unique_ptr<FunctionBlockExpression> {
			                                                return std::make_unique<FunctionBlockExpression>(FunctionDeclaration(
			                                                        t_keyword, name, v_types, {}, FunctionBody{ std::move(body) }));
		                                                }));
	};

	ExpressionPtr ep_braced;
	if (p_parser.peek().get_lexeme() == cloth::to_string(Operator::o_BRACE_OPEN)) {
		ep_braced = fn_braces(v_targets.front().get_token());
	}
	const bool b_functionBlock = dynamic_cast<const FunctionBlockExpression *>(ep_braced.get()) != nullptr;
	if (b_functionBlock && v_types.size() <= 1 && std::all_of(v_targets.begin(), v_targets.end(), [] (const ReceivingTarget &target) {
		    return std::holds_alternative<ReceivingTarget::Named>(target.value);
	    })) {
		std::vector<StatementPtr> v_bindings;
		v_bindings.reserve(v_targets.size());
		for (std::size_t st_index = 0; st_index < v_targets.size(); ++st_index) {
			const Token  &t_name         = v_targets[st_index].get_token();
			ExpressionPtr ep_initializer = ep_braced;
			if (st_index != 0) {
				// Each deferred binding has its own function body and AST identity.
				p_parser.i_currentIndex = i_initializerStart;
				ep_initializer          = fn_braces(t_name);
			}
			const TokenSpan ts_span = TokenSpan::span_through(t_keyword.get_span(), ep_initializer->get_span());
			v_bindings.push_back(std::make_shared<BindingDeclaration>(opt_type, t_name, std::move(ep_initializer), b_isMutable, ts_span));
		}
		return v_bindings;
	}

	ExpressionPtr ep_initializer;
	if (b_functionBlock) {
		ep_initializer = std::move(ep_braced);
	} else if (!ep_braced) {
		ep_initializer = p_parser.parseResults(nullptr, p_parser.peek().get_span(), allowWhileTrailer, /*multiline=*/false);
	} else {
		const TokenSpan ts_bracedStart = ep_braced->get_span();
		ExpressionPtr   ep_primary     = p_parser.p_expressionParser.continueFromPrimary(std::move(ep_braced), allowWhileTrailer);
		ep_initializer                 = p_parser.parseResults(std::move(ep_primary), ts_bracedStart, allowWhileTrailer, /*multiline=*/false);
	}

	const TokenSpan ts_span = TokenSpan::span_through(t_keyword.get_span(), p_parser.previousStatementEnd());
	if (v_targets.size() == 1 && std::holds_alternative<ReceivingTarget::Named>(v_targets.front().value) && v_types.size() <= 1) {
		return { std::make_shared<BindingDeclaration>(std::move(opt_type), v_targets.front().get_token(), std::move(ep_initializer), b_isMutable,
			                                      ts_span) };
	}
	return { std::make_shared<BindingGroupDeclaration>(std::move(v_types), std::move(v_targets), std::move(ep_initializer), b_isMutable, ts_span) };
}

}
