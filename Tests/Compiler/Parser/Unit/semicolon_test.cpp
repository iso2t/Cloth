// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/source_unit.hpp>
#include <ast/stmt/statements.hpp>
#include <cde/cloth_diagnostic.hpp>
#include <cde/diagnostic_code.hpp>
#include <cde/syntax_exception.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

SourceUnit parseStrict (const std::string &source) {
	Lexer lexer(source);
	return Parser(lexer, SourceGrammar::ExplicitSemicolons).parse();
}

const FunctionDeclaration &firstFunction (const SourceUnit &source) {
	return dynamic_cast<const FunctionDeclaration &>(*source.declarations.at(0));
}

std::vector<std::string> rangesOf (const ParsedSource &source, const SourceConstruct kind) {
	std::vector<std::string> spans;
	for (const SourceRange &range : source.ranges) {
		if (range.construct == kind) spans.push_back(range.span.toString());
	}
	return spans;
}

TEST (ParserSemicolons, WrappedSubtractionDistinguishesTwoGrammarsOnTheSameTokens) {
	Lexer                    lexer("pub fn<int> main() {\r\n    def n := 7\r\n        - 2;\r\n    return n;\r\n}");
	const std::vector<Token> tokens  = lexer.scanTokens();
	const SourceUnit         legacy  = Parser(tokens, SourceGrammar::Legacy).parse();
	const SourceUnit         strict  = Parser(tokens, SourceGrammar::ExplicitSemicolons).parse();
	const auto              &oldBody = std::get<BlockBody>(firstFunction(legacy).body.value);
	const auto              &newBody = std::get<BlockBody>(firstFunction(strict).body.value);
	ASSERT_EQ(oldBody.statements.size(), 3u);
	ASSERT_EQ(newBody.statements.size(), 2u);
	const auto &oldBinding = dynamic_cast<const BindingDeclaration &>(*oldBody.statements.at(0));
	const auto &newBinding = dynamic_cast<const BindingDeclaration &>(*newBody.statements.at(0));
	EXPECT_EQ(dynamic_cast<const IntegerLiteral &>(*oldBinding.initializer).value, IntegerValue{ 7 });
	EXPECT_EQ(dynamic_cast<const BinaryExpression &>(*newBinding.initializer).op.getOperator(), OMinus);
}

TEST (ParserSemicolons, UnbracedConditionalLeavesTheNextArgumentToTheCall) {
	Lexer                    lexer("fn<int> pick(bool ready) = pair(if (ready) 4 else 2, 3);");
	const std::vector<Token> tokens  = lexer.scanTokens();
	const SourceUnit         legacy  = Parser(tokens, SourceGrammar::Legacy).parse();
	const SourceUnit         strict  = Parser(tokens, SourceGrammar::ExplicitSemicolons).parse();
	const auto              &oldCall = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(firstFunction(legacy).body.value).expression);
	const auto              &newCall = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(firstFunction(strict).body.value).expression);
	ASSERT_EQ(oldCall.suppliedArguments.size(), 1u);
	ASSERT_EQ(newCall.suppliedArguments.size(), 2u);
	const auto &conditional = dynamic_cast<const IfExpression &>(*newCall.suppliedArguments.front().expression);
	if (conditional.otherwise) {
		const auto &arm = dynamic_cast<const ExpressionStatement &>(*conditional.otherwise->at(0));
		EXPECT_EQ(dynamic_cast<const IntegerLiteral &>(*arm.expression).value, IntegerValue{ 2 });
	} else {
		FAIL() << "Expected an else arm on the first argument";
	}
	EXPECT_EQ(dynamic_cast<const IntegerLiteral &>(*newCall.suppliedArguments.at(1).expression).value, IntegerValue{ 3 });

	const SourceUnit results = parseStrict("fn<int,int> choose(bool b) = if (b) { 4, 3; } else { 2, 1; };");
	const auto      &choice  = dynamic_cast<const IfExpression &>(*std::get<ExpressionBody>(firstFunction(results).body.value).expression);
	const auto      &tail    = dynamic_cast<const ExpressionStatement &>(*choice.branches.at(0).statements.at(0));
	EXPECT_EQ(dynamic_cast<const ResultListExpression &>(*tail.expression).values.size(), 2u);
}

TEST (ParserSemicolons, LegacyRangesKeepGroupsParenthesesAndTrailingComments) {
	Lexer              lexer("fn f() {\r\n  int a, b = { return (7) } // grouped\r\n  return // bare\r\n  work()\r\n  Point p := {0, 0}\r\n}");
	const ParsedSource source = Parser(lexer, SourceGrammar::Legacy).parseWithRanges();
	const auto        &body   = std::get<BlockBody>(firstFunction(source.unit).body.value);
	ASSERT_EQ(body.statements.size(), 5u);
	EXPECT_EQ(dynamic_cast<const ReturnStatement &>(*body.statements.at(2)).value, nullptr);
	EXPECT_EQ(rangesOf(source, SourceConstruct::Statement), (std::vector<std::string>{ "2:3-2:28", "2:16-2:26", "3:3-3:9", "4:3-4:9", "5:3-5:20" }));
	EXPECT_EQ(rangesOf(source, SourceConstruct::DeferredInitializer), (std::vector<std::string>{ "2:14-2:28" }));
	EXPECT_EQ(rangesOf(source, SourceConstruct::StructLiteral), (std::vector<std::string>{ "5:14-5:20" }));
	ASSERT_FALSE(source.ranges.empty());
	ASSERT_NE(source.ranges.front().span.source, nullptr);
	EXPECT_NE(source.ranges.front().span.source->getContents().find("// grouped\r\n"), std::string::npos);
}

TEST (ParserSemicolons, DeclarationAndMemberListsOwnTheirTerminators) {
	EXPECT_NO_THROW(parseStrict(R"(using app
	/* across
	   lines */ .values
	: A
	, B;
	pub typedef<interface> Tag;
	typedef<class> Ticket(pub def<int> number) : Tag;
	typedef<struct> Empty;
	typedef<struct> Point { int x, int y; mt<int> sum() = self.x + self.y; }
	typedef<enum> Mode { Off, On; }
	typedef<interface> Sized { pub mt<int> size(); pub mt<bool> empty() = default (self.size() == 0);
		mt<int> zero() = default { return 0; } }
	typedef<class> Machine {
		typedef<class> Nested; pub def<int> count = 0;
		default state<void> Idle; state Ready;
	}
	typedef<state<void>> Ready for Machine;
	fn reset() {} fn stop() = return; fn<int> value() = 2; )"));
	for (const char *source :
	     { "pub typedef<class> MyClass", "typedef<interface> Tag", "typedef<enum> Mode;", "typedef<enum> Mode {}", "typedef<enum> Mode { Off }",
	       "typedef<struct> S { int x }", "fn reset() {};", "fn reset();", "typedef<class> C { def<int> count = 1 }", "fn<int> value() = 2" }) {
		SCOPED_TRACE(source);
		EXPECT_THROW(parseStrict(source), cde::SyntaxException);
	}
}

TEST (ParserSemicolons, ControlFlowAndBraceInitializersOwnSeparateEndings) {
	const SourceUnit                     source = parseStrict(R"(fn run(bool b) {
		Point origin := {0, 0}; Marker empty := {}; Box single := {(1)};
		int later := { return 42; };
		if (b) first(); else second();
		if (b) if (b) first(); else second();
		while (b) step(); do step(); while (b); do { step(); } while (b);
		for (int i <- 0..3; i++) { if (b) continue; else break; }
		return if (b) { 42; } else { 7; };
	})");
	const auto                          &body   = std::get<BlockBody>(firstFunction(source).body.value);
	constexpr std::array<std::size_t, 3> fieldCounts{ 2, 0, 1 };
	for (std::size_t index = 0; index < fieldCounts.size(); ++index) {
		const auto &binding = dynamic_cast<const BindingDeclaration &>(*body.statements.at(index));
		EXPECT_EQ(dynamic_cast<const StructLiteral &>(*binding.initializer).fields.size(), fieldCounts[index]);
	}
	const auto &deferred = dynamic_cast<const BindingDeclaration &>(*body.statements.at(3));
	EXPECT_NE(dynamic_cast<const FunctionBlockExpression *>(deferred.initializer.get()), nullptr);
	const auto &nested = dynamic_cast<const IfExpression &>(*dynamic_cast<const ExpressionStatement &>(*body.statements.at(5)).expression);
	EXPECT_FALSE(nested.otherwise);
	EXPECT_TRUE(
	        dynamic_cast<const IfExpression &>(*dynamic_cast<const ExpressionStatement &>(*nested.branches.at(0).statements.at(0)).expression).otherwise);

	EXPECT_NO_THROW(parseStrict("fn stop(bool b) = if (b) { return; };"));
	EXPECT_NO_THROW(parseStrict("fn<int> early(bool b) = if (b) return 7; else 2;"));
	for (const char *invalid : { "fn f() { ; }", "fn f() { act();; }", "fn f() { while (true); }", "fn f() { do {} while (true) }",
	                             "fn f() { int later := { return 42; } }", "fn f(bool b) = if (b) { return; }" }) {
		SCOPED_TRACE(invalid);
		EXPECT_THROW(parseStrict(invalid), cde::SyntaxException);
	}
}

TEST (ParserSemicolons, HeaderOnlyTypesPreserveFieldsContractsAndStateAttachments) {
	for (const std::string ending : { ";", " {}" }) {
		SCOPED_TRACE(ending);
		const std::string source = "typedef<interface> Tag" + ending + " typedef<class> Ticket(pub def<int> number) : Tag" + ending +
		                           " typedef<struct> Marker" + ending + " pub typedef<class> Machine { pub default state<void> Idle" + ending +
		                           " pub state Ready; } typedef<state<void>> pub Ready for Machine" + ending +
		                           " fn<int> number(Ticket t) = t.number; fn<bool> entered(Machine m) { m -> Ready; return m is Ready; }";
		EXPECT_NO_THROW((CheckedProgram{ SymbolTable(parseStrict(source)) }));
	}
	EXPECT_THROW((CheckedProgram{ SymbolTable(parseStrict("typedef<class> Ticket; typedef<class> Ticket {}")) }), cde::ClothDiagnostic);
	EXPECT_THROW((CheckedProgram{ SymbolTable(parseStrict("typedef<interface> I { pub mt<int> value(); } typedef<class> C : I;")) }), cde::ClothDiagnostic);
}

TEST (ParserSemicolons, UnterminatedClassHeaderAtEofHasAnInsertionPoint) {
	const std::string source = "pub typedef<class> MyClass";
	try {
		parseStrict(source);
		FAIL() << "Expected the class header to need a terminator";
	} catch (const cde::SyntaxException &error) {
		EXPECT_EQ(error.getCode(), cde::DiagnosticCode::ExpectedSemicolon);
		EXPECT_EQ(error.getMessage(), "Expected '{' or ';' after class header");
		ASSERT_EQ(error.getSourceLabels().size(), 1u);
		const TokenSpan &span = error.getSourceLabels().front().span;
		EXPECT_EQ(span.startCol, source.size() + 1);
		EXPECT_EQ(span.endCol, span.startCol);
	}
}

TEST (ParserSemicolons, TransitiveImportsUseTheEntryGrammar) {
	const std::filesystem::path entry  = std::filesystem::path(__FILE__).parent_path().parent_path() / "Inputs" / "semicolon_entry.co";
	const std::string           source = "using semicolon_module: Ticket;\npub fn<int> main() = 42;";
	Lexer                       lexer(SourceFile(entry, source));
	const std::vector<Token>    tokens = lexer.scanTokens();
	const CheckedProgram        program{ SymbolTable(Parser(tokens, SourceGrammar::ExplicitSemicolons).parse()) };
	ASSERT_EQ(program.units.size(), 3u);
	for (const auto &unit : program.units) {
		EXPECT_EQ(unit->getSourceUnit().grammar, SourceGrammar::ExplicitSemicolons);
	}
	EXPECT_THROW((CheckedProgram{ SymbolTable(Parser(tokens, SourceGrammar::Legacy).parse()) }), cde::SyntaxException);
}

TEST (ParserSemicolons, PhysicalWhitespacePreservesGenericAdjacencyAndNewlinesInsideLiterals) {
	const SourceUnit applied = parseStrict("fn<T> f(T x) = first<T>(\r\nx\r\n);");
	const auto      &call    = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(firstFunction(applied).body.value).expression);
	EXPECT_TRUE(call.typeArguments);
	for (const char *expression : { "first <T>(x)", "first\n<T>(x)", "first<T> (x)", "first<T>\r\n(x)" }) {
		SCOPED_TRACE(expression);
		const SourceUnit comparison = parseStrict(std::string("fn<T> f(T x) = ") + expression + ";");
		EXPECT_NE(dynamic_cast<const BinaryExpression *>(std::get<ExpressionBody>(firstFunction(comparison).body.value).expression.get()), nullptr);
	}
	const SourceUnit literal = parseStrict("fn<string> text() = \"\"\"first\nsecond\"\"\";");
	EXPECT_EQ(dynamic_cast<const StringLiteral &>(*std::get<ExpressionBody>(firstFunction(literal).body.value).expression).value, "first\nsecond");
}

TEST (ParserSemicolons, WrappedReturnCallsAndIndexingKeepTheirValues) {
	const SourceUnit source = parseStrict("fn<int, int> pick(Things\n items) { return // keep reading\r\n"
	                                      "items\n .values\n ()\n [0]\n ,\n items\n .values()[1]; }");
	const auto      &body   = std::get<BlockBody>(firstFunction(source).body.value);
	ASSERT_EQ(body.statements.size(), 1u);
	const auto &returned = dynamic_cast<const ReturnStatement &>(*body.statements.front());
	ASSERT_NE(returned.value, nullptr);
	const auto &results = dynamic_cast<const ResultListExpression &>(*returned.value);
	ASSERT_EQ(results.values.size(), 2u);
	for (const ExpressionPtr &value : results.values) {
		EXPECT_NE(dynamic_cast<const IndexExpression *>(value.get()), nullptr);
	}
}

TEST (ParserSemicolons, MissingTerminatorLabelsTheExclusiveUtf8End) {
	const std::string source = "fn f() {\r\n  return caf\xc3\xa9 // trailing comment\r\n}";
	try {
		parseStrict(source);
		FAIL() << "Expected a missing semicolon";
	} catch (const cde::SyntaxException &error) {
		EXPECT_EQ(error.getCode(), cde::DiagnosticCode::ExpectedSemicolon);
		EXPECT_EQ(error.getMessage(), "Expected ';' after return statement");
		ASSERT_EQ(error.getSourceLabels().size(), 1u);
		const TokenSpan &span = error.getSourceLabels().front().span;
		EXPECT_EQ(span.toString(), "2:15-2:15");
		ASSERT_NE(span.source, nullptr);
		EXPECT_EQ(span.source->getContents(), source);
	}
}

}
}
