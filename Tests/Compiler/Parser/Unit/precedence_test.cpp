// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>
#include <variant>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/source_unit.hpp>
#include <ast/stmt/statements.hpp>
#include <cde/syntax_exception.hpp>
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

TEST (ParserPrecedence, PreservesComparisonsThatResembleTypedBindings) {
	Lexer                      l_lexer("fn probe() { left < middle > right, tail }");
	Parser                     p_parser(l_lexer);
	const SourceUnit           su_unit     = p_parser.parse();
	const FunctionDeclaration &fd_function = dynamic_cast<const FunctionDeclaration &>(*su_unit.declarations.front());
	const BlockBody           &bb_body     = std::get<BlockBody>(fd_function.body.value);
	ASSERT_EQ(bb_body.statements.size(), 1u);
	const ExpressionStatement  &es_statement = dynamic_cast<const ExpressionStatement &>(*bb_body.statements.front());
	const ResultListExpression &rl_results   = dynamic_cast<const ResultListExpression &>(*es_statement.expression);
	ASSERT_EQ(rl_results.values.size(), 2u);
	const BinaryExpression &be_comparison = dynamic_cast<const BinaryExpression &>(*rl_results.values.front());
	EXPECT_EQ(be_comparison.op.get_operator(), oGreater);
}

TEST (ParserDeclarations, RejectsBracketedLocalTypesAndMisplacedOverrideModifiers) {
	for (const char *s_source : { "fn f() { def<int> x = 1 }", "fn f() { for (def<int> i <- 0..2) {} }", "typedef<class> C { override: pub mt f() {} }",
	                              "typedef<class> C { override pub mt f() {} }", "typedef<class> C { pub override: mt f() {} }" }) {
		SCOPED_TRACE(s_source);
		Lexer  l_lexer(s_source);
		Parser p_parser(l_lexer);
		EXPECT_THROW(p_parser.parse(), cde::SyntaxException);
	}
}

TEST (ParserDeclarations, PlacesVirtualBetweenVisibilityAndTheMethodKeyword) {
	Lexer                    l_lexer("typedef<class> C {\n    pub virtual mt f() {}\n}");
	Parser                   p_parser(l_lexer);
	const SourceUnit         su_unit   = p_parser.parse();
	const ClassDeclaration  &cd_class  = dynamic_cast<const ClassDeclaration &>(*su_unit.declarations.at(0));
	const MethodDeclaration &md_method = std::get<MethodDeclaration>(cd_class.members.at(0).value);
	ASSERT_TRUE(md_method.virtualPrefix);
	EXPECT_EQ(md_method.virtualPrefix->to_string(), "2:9-2:16");
	EXPECT_EQ(md_method.get_span().to_string(), "2:5-2:26");

	for (const char *s_source :
	     { "typedef<class> C { virtual: pub mt f() {} }", "typedef<class> C { virtual pub mt f() {} }", "typedef<class> C { pub virtual: mt f() {} }",
	       "typedef<class> C { pub virtual override mt f() {} }", "typedef<class> C { pub override virtual mt f() {} }" }) {
		SCOPED_TRACE(s_source);
		Lexer  l_invalid(s_source);
		Parser p_invalid(l_invalid);
		EXPECT_THROW(p_invalid.parse(), cde::SyntaxException);
	}
}
}
}
