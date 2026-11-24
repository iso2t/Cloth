// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ast/decl/function_declaration.hpp>
#include <ast/expr/class_expression.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/member_call_expression.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/type/type_reference.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth {

void visitOperands (const Expression &node, const std::function<void(const Expression &)> &visit,
                    const std::function<void(const std::vector<StatementPtr> &)> &visitBlock) {
	if (const auto *cs_call = dynamic_cast<const CallSite *>(&node)) {
		if (const auto *mc_member = dynamic_cast<const MemberCallExpression *>(cs_call))
			visit(*mc_member->receiver);
		else if (const auto *sc_state = dynamic_cast<const StateCallExpression *>(cs_call))
			visit(*sc_state->receiver);
		else if (const auto *cc_create = dynamic_cast<const ClassCreationExpression *>(cs_call))
			visit(*cc_create->receiver);
		for (const CallArgument &ca_argument : cs_call->suppliedArguments) visit(*ca_argument.expression);
	} else if (const auto *fa_field = dynamic_cast<const FieldAccessExpression *>(&node))
		visit(*fa_field->receiver);
	else if (const auto *ce_cast = dynamic_cast<const CastExpression *>(&node))
		visit(*ce_cast->operand);
	else if (const auto *tc_test = dynamic_cast<const TypeCheckExpression *>(&node))
		visit(*tc_test->operand);
	else if (const auto *pa_provider = dynamic_cast<const ProviderAccessExpression *>(&node))
		visit(*pa_provider->receiver);
	else if (const auto *ie_index = dynamic_cast<const IndexExpression *>(&node)) {
		visit(*ie_index->array);
		visit(*ie_index->index);
	} else if (const auto *al_array = dynamic_cast<const ArrayLiteral *>(&node)) {
		for (const ExpressionPtr &ep_item : al_array->elements) visit(*ep_item);
	} else if (const auto *sl_struct = dynamic_cast<const StructLiteral *>(&node)) {
		for (const ExpressionPtr &ep_item : sl_struct->fields) visit(*ep_item);
	} else if (const auto *rl_list = dynamic_cast<const ResultListExpression *>(&node)) {
		for (const ExpressionPtr &ep_item : rl_list->values) visit(*ep_item);
	} else if (const auto *is_string = dynamic_cast<const InterpolatedString *>(&node)) {
		for (const ExpressionPtr &ep_item : is_string->parts) visit(*ep_item);
	} else if (const auto *be_binary = dynamic_cast<const BinaryExpression *>(&node)) {
		visit(*be_binary->left);
		visit(*be_binary->right);
	} else if (const auto *ue_unary = dynamic_cast<const UnaryExpression *>(&node))
		visit(*ue_unary->operand);
	else if (const auto *ee_elvis = dynamic_cast<const ElvisExpression *>(&node)) {
		visit(*ee_elvis->left);
		visit(*ee_elvis->fallback);
	} else if (const auto *ga_access = dynamic_cast<const GuardedAccessExpression *>(&node)) {
		visit(*ga_access->getReceiver());
		visit(*ga_access->continuation);
	} else if (const auto *ie_conditional = dynamic_cast<const IfExpression *>(&node)) {
		for (const ConditionalBranch &cb_branch : ie_conditional->branches) {
			visit(*cb_branch.condition);
			visitBlock(cb_branch.statements);
		}
		if (ie_conditional->otherwise) visitBlock(*ie_conditional->otherwise);
	} else if (const auto *cd_delete = dynamic_cast<const ClassDeletionExpression *>(&node))
		visit(*cd_delete->receiver);
	else if (const auto *st_transition = dynamic_cast<const StateTransitionExpression *>(&node)) {
		visit(*st_transition->receiver);
		if (st_transition->payload) visit(*st_transition->payload);
	}
}

NameExpression::NameExpression (const Token &name) : name(name) {
}

FunctionBlockExpression::FunctionBlockExpression (FunctionDeclaration declaration) : declaration(std::move(declaration)) {
}

TokenSpan CallArgument::getSpan () const {
	const TokenSpan ts_expression = expression->getSpan();
	return label ? TokenSpan::spanThrough(label->getSpan(), ts_expression) : ts_expression;
}

CallSite::CallSite (const Token &name, std::vector<CallArgument> suppliedArguments) : name(name), suppliedArguments(std::move(suppliedArguments)) {
}

std::vector<ExpressionPtr> CallSite::getArguments () const {
	std::vector<ExpressionPtr> v_arguments;
	v_arguments.reserve(suppliedArguments.size());
	for (const CallArgument &ca_argument : suppliedArguments) {
		v_arguments.push_back(ca_argument.expression);
	}
	return v_arguments;
}

CallExpression::CallExpression (const Token &name, std::vector<CallArgument> suppliedArguments, TokenSpan span, std::optional<TypeArgumentList> typeArguments) :
        CallSite(name, std::move(suppliedArguments)),
        span(std::move(span)),
        typeArguments(std::move(typeArguments)) {
}

ArrayLiteral::ArrayLiteral (std::vector<ExpressionPtr> elements, TokenSpan span) : elements(std::move(elements)), span(std::move(span)) {
}

ResultListExpression::ResultListExpression (std::vector<ExpressionPtr> values, TokenSpan span) : values(std::move(values)), span(std::move(span)) {
}

IndexExpression::IndexExpression (ExpressionPtr array, ExpressionPtr index, TokenSpan span) :
        array(std::move(array)),
        index(std::move(index)),
        span(std::move(span)) {
}

UnaryExpression::UnaryExpression (const Token &op, ExpressionPtr operand) : op(op), operand(std::move(operand)) {
}

BinaryExpression::BinaryExpression (ExpressionPtr left, const Token &op, ExpressionPtr right) : left(std::move(left)), op(op), right(std::move(right)) {
}

std::shared_ptr<const BinaryExpression> compoundExpression (const ExpressionPtr &target, const Token &op, const ExpressionPtr &value) {
	if (!value || op.getOperator() == OAssign) return nullptr;
	const Operator o_base = compoundAssignmentBase(op.getLexeme());
	const Token    t_base(std::string(toString(o_base)), o_base, op.getSpan());
	return std::make_shared<BinaryExpression>(target, t_base, value);
}

const BinaryExpression *rangeOrNull (const Expression &expression) {
	const auto *const be_binary = dynamic_cast<const BinaryExpression *>(&expression);
	if (!be_binary) {
		return nullptr;
	}
	const std::string &s_operator = be_binary->op.getLexeme();
	if (s_operator == toString(ORange) || s_operator == toString(ORangeInclusive)) {
		return be_binary;
	}
	return nullptr;
}

ElvisExpression::ElvisExpression (ExpressionPtr left, const Token &op, ExpressionPtr fallback) : left(std::move(left)), op(op), fallback(std::move(fallback)) {
}

GuardedReceiver::GuardedReceiver (ExpressionPtr source) : source(std::move(source)) {
}

GuardedAccessExpression::GuardedAccessExpression (const Token &op, std::shared_ptr<const GuardedReceiver> payload, ExpressionPtr continuation) :
        op(op),
        payload(std::move(payload)),
        continuation(std::move(continuation)) {
}

NullLiteral::NullLiteral (TokenSpan span) : span(std::move(span)) {
}

CastExpression::CastExpression (ExpressionPtr operand, const Token &op, TypeReference target) : operand(std::move(operand)), op(op), target(std::move(target)) {
}

TypeCheckExpression::TypeCheckExpression (ExpressionPtr operand, const Token &op, TypeReference target,
                                          std::shared_ptr<const FieldAccessExpression> valueCandidate) :
        operand(std::move(operand)),
        op(op),
        target(std::move(target)),
        valueCandidate(std::move(valueCandidate)) {
}

IfExpression::IfExpression (const Token &keyword, std::vector<ConditionalBranch> branches, std::optional<std::vector<StatementPtr>> otherwise, TokenSpan span,
                            std::optional<TokenSpan> otherwiseSpan) :
        keyword(keyword),
        branches(std::move(branches)),
        otherwise(std::move(otherwise)),
        span(std::move(span)),
        otherwiseSpan(std::move(otherwiseSpan)) {
}

IntegerLiteral::IntegerLiteral (IntegerValue value, const Token &token) : value(std::move(value)), token(token) {
}

FloatingPointLiteral::FloatingPointLiteral (std::string value, const Token &token) : value(std::move(value)), token(token) {
}

BooleanLiteral::BooleanLiteral (const bool value, TokenSpan span) : value(value), span(std::move(span)) {
}

CharacterLiteral::CharacterLiteral (const char32_t value, TokenSpan span) : value(value), span(std::move(span)) {
}

StringLiteral::StringLiteral (std::string value, TokenSpan span) : value(std::move(value)), span(std::move(span)) {
}

InterpolatedString::InterpolatedString (std::vector<ExpressionPtr> parts, TokenSpan span) : parts(std::move(parts)), span(std::move(span)) {
}

}
