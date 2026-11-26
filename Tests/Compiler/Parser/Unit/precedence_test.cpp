// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>
#include <string>
#include <variant>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/source_unit.hpp>
#include <ast/stmt/statements.hpp>
#include <cde/diagnostic_code.hpp>
#include <cde/syntax_exception.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>
#include "ast/decl/struct_declaration.hpp"

namespace cloth {
namespace {

TEST (ParserPrecedence, PowerBindsBeforeUnaryMinus) {
	Lexer            l_lexer("fn<int> value() = -2 ** 2;");
	Parser           p_parser(l_lexer);
	const SourceUnit su_unit = p_parser.parse();

	ASSERT_EQ(su_unit.declarations.size(), 1u);
	const auto *fd_function = dynamic_cast<const FunctionDeclaration *>(su_unit.declarations.front().get());
	ASSERT_NE(fd_function, nullptr);
	const ExpressionBody *eb_body = std::get_if<ExpressionBody>(&fd_function->body.value);
	ASSERT_NE(eb_body, nullptr);

	const auto *ue_minus = dynamic_cast<const UnaryExpression *>(eb_body->expression.get());
	ASSERT_NE(ue_minus, nullptr) << "Expected unary minus at the root of -2 ** 2";
	EXPECT_EQ(ue_minus->op.getOperator(), OMinus);
	const auto *be_power = dynamic_cast<const BinaryExpression *>(ue_minus->operand.get());
	ASSERT_NE(be_power, nullptr);
	EXPECT_EQ(be_power->op.getOperator(), OMulMul);

	const auto *il_base     = dynamic_cast<const IntegerLiteral *>(be_power->left.get());
	const auto *il_exponent = dynamic_cast<const IntegerLiteral *>(be_power->right.get());
	ASSERT_NE(il_base, nullptr);
	ASSERT_NE(il_exponent, nullptr);
	EXPECT_EQ(il_base->value, IntegerValue{ 2 });
	EXPECT_EQ(il_exponent->value, IntegerValue{ 2 });
}

TEST (ParserPrecedence, PreservesComparisonsThatResembleTypedBindings) {
	Lexer                      l_lexer("fn probe() { left < middle > right, tail; }");
	Parser                     p_parser(l_lexer);
	const SourceUnit           su_unit     = p_parser.parse();
	const auto &fd_function = dynamic_cast<const FunctionDeclaration &>(*su_unit.declarations.front());
	const auto           &bb_body     = std::get<BlockBody>(fd_function.body.value);
	ASSERT_EQ(bb_body.statements.size(), 1u);
	const auto  &es_statement = dynamic_cast<const ExpressionStatement &>(*bb_body.statements.front());
	const auto &rl_results   = dynamic_cast<const ResultListExpression &>(*es_statement.expression);
	ASSERT_EQ(rl_results.values.size(), 2u);
	const auto &be_comparison = dynamic_cast<const BinaryExpression &>(*rl_results.values.front());
	EXPECT_EQ(be_comparison.op.getOperator(), OGreater);
}

TEST (ParserDeclarations, RejectsBracketedLocalTypesAndMisplacedOverrideModifiers) {
	for (const char *s_source : { "fn f() { def<int> x = 1; }", "fn f() { for (def<int> i <- 0..2) {} }", "typedef<class> C { override: pub mt f() {} }",
	                              "typedef<class> C { override pub mt f() {} }", "typedef<class> C { pub override: mt f() {} }" }) {
		SCOPED_TRACE(s_source);
		Lexer  l_lexer(s_source);
		Parser p_parser(l_lexer);
		EXPECT_THROW(p_parser.parse(), cde::SyntaxException);
	}
}

TEST (ParserDeclarations, ReportsMissingInitializerForUnambiguousTypedLocals) {
	for (const char *s_declaration : { "Wibble x", "Outer.Item? value", "int<12> n", "Item[2] items", "Box<T> box, bool ok" }) {
		SCOPED_TRACE(s_declaration);
		Lexer  l_lexer(std::string("fn f() { ") + s_declaration + "; }");
		Parser p_parser(l_lexer);
		try {
			p_parser.parse();
			FAIL() << "Expected a missing initializer";
		} catch (const cde::SyntaxException &error) {
			EXPECT_EQ(error.getCode(), cde::DiagnosticCode::ExpectedToken);
			EXPECT_EQ(error.getMessage(), "Expected '=' or ':=', found ';'");
		}
	}
}

TEST (ParserDeclarations, PlacesVirtualBetweenVisibilityAndTheMethodKeyword) {
	Lexer                    l_lexer("typedef<class> C {\n    pub virtual mt f() {}\n}");
	Parser                   p_parser(l_lexer);
	const SourceUnit         su_unit   = p_parser.parse();
	const auto  &cd_class  = dynamic_cast<const ClassDeclaration &>(*su_unit.declarations.at(0));
	const auto &md_method = std::get<MethodDeclaration>(cd_class.members.at(0).value);
	ASSERT_TRUE(md_method.virtualPrefix);
	EXPECT_EQ(md_method.virtualPrefix->toString(), "2:9-2:16");
	EXPECT_EQ(md_method.getSpan().toString(), "2:5-2:26");

	for (const char *s_source :
	     { "typedef<class> C { virtual: pub mt f() {} }", "typedef<class> C { virtual pub mt f() {} }", "typedef<class> C { pub virtual: mt f() {} }",
	       "typedef<class> C { pub virtual override mt f() {} }", "typedef<class> C { pub override virtual mt f() {} }" }) {
		SCOPED_TRACE(s_source);
		Lexer  l_invalid(s_source);
		Parser p_invalid(l_invalid);
		EXPECT_THROW(p_invalid.parse(), cde::SyntaxException);
	}
}

TEST (ParserDeclarations, RecordsInitialStateModifiersOnDeclarationsAndAttachments) {
	Lexer                         l_lexer(R"(typedef<class> Lamp {
    pub default state<void> Ready {}
}
typedef<class> Meter {
    pub default(1) state Busy;
}
typedef<state<int>> pub Busy for Meter {}
)");
	Parser                        p_parser(l_lexer);
	const SourceUnit              su_unit  = p_parser.parse();
	const auto       &cd_lamp  = dynamic_cast<const ClassDeclaration &>(*su_unit.declarations.at(0));
	const auto &ns_ready = dynamic_cast<const NestedStateDeclaration &>(*std::get<StateClassMemberPtr>(cd_lamp.members.at(0).value));
	ASSERT_TRUE(ns_ready.initial);
	EXPECT_FALSE(ns_ready.initial->payload);
	EXPECT_EQ(ns_ready.initial->span.toString(), "2:9-2:16");
	EXPECT_EQ(ns_ready.getSpan().toString(), "2:5-2:37");

	const auto           &cd_meter = dynamic_cast<const ClassDeclaration &>(*su_unit.declarations.at(1));
	const auto &sa_busy =
	        dynamic_cast<const StateAttachmentDeclaration &>(*std::get<StateClassMemberPtr>(cd_meter.members.at(0).value));
	ASSERT_TRUE(sa_busy.initial);
	ASSERT_TRUE(sa_busy.initial->payload);
	EXPECT_EQ(sa_busy.initial->payload->getSpan().toString(), "5:17-5:18");
	EXPECT_EQ(sa_busy.initial->span.toString(), "5:9-5:19");
	EXPECT_EQ(sa_busy.getSpan().toString(), "5:5-5:30");
}

TEST (ParserDeclarations, RejectsMisplacedInitialStateModifiers) {
	for (const char *s_member :
	     { "default: state<void> Ready {}", "default: (1) pub state Busy;", "pub default: state<void> Ready {}", "default pub state<void> Ready {}",
	       "default(1) pub state Busy;", "pub default mt read() {}", "pub default state<int> ::read();" }) {
		SCOPED_TRACE(s_member);
		Lexer  l_lexer(std::string("typedef<class> C { ") + s_member + " }");
		Parser p_parser(l_lexer);
		EXPECT_THROW(p_parser.parse(), cde::SyntaxException);
	}
}
}
}
