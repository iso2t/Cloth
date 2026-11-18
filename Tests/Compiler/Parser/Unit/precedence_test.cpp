// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>
#include <variant>

#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/source_unit.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

TEST (ParserPrecedence, PowerBindsBeforeUnaryMinus) {
	Lexer            l_lexer("fn<int> value() = -2 ** 2");
	Parser           p_parser(l_lexer);
	const SourceUnit su_unit = p_parser.parse();

	ASSERT_EQ(su_unit.declarations.size(), 1u);
	const FunctionDeclaration *fd_function = dynamic_cast<const FunctionDeclaration *>(su_unit.declarations.front().get());
	ASSERT_NE(fd_function, nullptr);
	const ExpressionBody *eb_body = std::get_if<ExpressionBody>(&fd_function->body.value);
	ASSERT_NE(eb_body, nullptr);

	const UnaryExpression *ue_minus = dynamic_cast<const UnaryExpression *>(eb_body->expression.get());
	ASSERT_NE(ue_minus, nullptr) << "Expected unary minus at the root of -2 ** 2";
	EXPECT_EQ(ue_minus->op.get_operator(), oMinus);
	const BinaryExpression *be_power = dynamic_cast<const BinaryExpression *>(ue_minus->operand.get());
	ASSERT_NE(be_power, nullptr);
	EXPECT_EQ(be_power->op.get_operator(), oMulMul);

	const IntegerLiteral *il_base     = dynamic_cast<const IntegerLiteral *>(be_power->left.get());
	const IntegerLiteral *il_exponent = dynamic_cast<const IntegerLiteral *>(be_power->right.get());
	ASSERT_NE(il_base, nullptr);
	ASSERT_NE(il_exponent, nullptr);
	EXPECT_EQ(il_base->value, IntegerValue{ 2 });
	EXPECT_EQ(il_exponent->value, IntegerValue{ 2 });
}

}
}
