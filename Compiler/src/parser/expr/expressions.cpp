// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <parser/decl/declarations.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/stmt/statement.hpp>
#include <parser/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

NameExpression::NameExpression (const Token &name) : name(name) {
}

FunctionBlockExpression::FunctionBlockExpression (FunctionDeclaration declaration) : declaration(std::move(declaration)) {
}

TokenSpan CallArgument::get_span () const {
	const TokenSpan ts_expression = expression->get_span();
	return label ? TokenSpan::span_through(label->get_span(), ts_expression) : ts_expression;
}

CallSite::CallSite (const Token &name, std::vector<CallArgument> suppliedArguments) : name(name), suppliedArguments(std::move(suppliedArguments)) {
}

std::vector<ExpressionPtr> CallSite::get_arguments () const {
	std::vector<ExpressionPtr> v_arguments;
	v_arguments.reserve(suppliedArguments.size());
	for (const CallArgument &ca_argument : suppliedArguments) {
		v_arguments.push_back(ca_argument.expression);
	}
	return v_arguments;
}

CallExpression::CallExpression (const Token &name, std::vector<CallArgument> suppliedArguments, TokenSpan span, std::optional<TypeArgumentList> typeArguments) :
        CallSite(name, std::move(suppliedArguments)),
        span(span),
        typeArguments(std::move(typeArguments)) {
}

ArrayLiteral::ArrayLiteral (std::vector<ExpressionPtr> elements, TokenSpan span) : elements(std::move(elements)), span(span) {
}

ResultListExpression::ResultListExpression (std::vector<ExpressionPtr> values, TokenSpan span) : values(std::move(values)), span(span) {
}

IndexExpression::IndexExpression (ExpressionPtr array, ExpressionPtr index, TokenSpan span) : array(std::move(array)), index(std::move(index)), span(span) {
}

UnaryExpression::UnaryExpression (const Token &op, ExpressionPtr operand) : op(op), operand(std::move(operand)) {
}

BinaryExpression::BinaryExpression (ExpressionPtr left, const Token &op, ExpressionPtr right) : left(std::move(left)), op(op), right(std::move(right)) {
}

const BinaryExpression *rangeOrNull (const Expression &expression) {
	const BinaryExpression *be_binary = dynamic_cast<const BinaryExpression *>(&expression);
	if (!be_binary) {
		return nullptr;
	}
	const std::string &s_operator = be_binary->op.get_lexeme();
	if (s_operator == cloth::to_string(Operator::o_RANGE) || s_operator == cloth::to_string(Operator::o_RANGE_INCLUSIVE)) {
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

NullLiteral::NullLiteral (TokenSpan span) : span(span) {
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
        span(span),
        otherwiseSpan(otherwiseSpan) {
}

IntegerLiteral::IntegerLiteral (long long value, const Token &token) : value(std::move(value)), token(token) {
}

FloatingPointLiteral::FloatingPointLiteral (std::string value, const Token &token) : value(std::move(value)), token(token) {
}

BooleanLiteral::BooleanLiteral (bool value, TokenSpan span) : value(value), span(span) {
}

CharacterLiteral::CharacterLiteral (char32_t value, TokenSpan span) : value(value), span(span) {
}

StringLiteral::StringLiteral (std::string value, TokenSpan span) : value(std::move(value)), span(span) {
}

InterpolatedString::InterpolatedString (std::vector<ExpressionPtr> parts, TokenSpan span) : parts(std::move(parts)), span(span) {
}

}
