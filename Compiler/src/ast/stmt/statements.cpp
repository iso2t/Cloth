// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <token/token.hpp>

namespace cloth {

namespace {

std::shared_ptr<const BinaryExpression> compoundExpression (const ExpressionPtr &target, const Token &op, const ExpressionPtr &value) {
	if (!value || op.get_lexeme() == cloth::to_string(Operator::o_ASSIGN)) {
		return nullptr;
	}
	const Operator o_base = compound_assignment_base(op.get_lexeme());
	const Token    t_base(std::string(cloth::to_string(o_base)), op.get_type(), op.get_span());
	return std::make_shared<BinaryExpression>(target, t_base, value);
}

}

ElementAssignmentStatement::ElementAssignmentStatement (std::shared_ptr<const IndexExpression> target, const Token &op, ExpressionPtr value) :
        target(std::move(target)),
        op(op),
        value(std::move(value)),
        compound(compoundExpression(this->target, op, this->value)) {
}

FieldWriteStatement::FieldWriteStatement (ExpressionPtr target, const Token &op, ExpressionPtr value) :
        target(std::move(target)),
        op(op),
        value(std::move(value)),
        compound(compoundExpression(this->target, op, this->value)) {
}

std::pair<ExpressionPtr, ExpressionPtr> updateOperands (const Statement &statement) {
	if (const AssignmentStatement *as_assignment = dynamic_cast<const AssignmentStatement *>(&statement)) {
		const BinaryExpression *be_compound = as_assignment->get_compound();
		if (!be_compound) {
			throw std::logic_error("Expected a compound assignment.");
		}
		return { be_compound->left, be_compound->right };
	}
	if (const UpdateStatement *us_update = dynamic_cast<const UpdateStatement *>(&statement)) {
		return { us_update->target, nullptr };
	}
	if (const FieldWriteStatement *fws_write = dynamic_cast<const FieldWriteStatement *>(&statement)) {
		return { fws_write->target, fws_write->value };
	}
	if (const ElementAssignmentStatement *eas_assignment = dynamic_cast<const ElementAssignmentStatement *>(&statement)) {
		return { eas_assignment->target, eas_assignment->value };
	}
	throw std::logic_error("Expected an update statement.");
}

}
