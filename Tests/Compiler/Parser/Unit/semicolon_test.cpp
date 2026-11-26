// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

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

SourceUnit parseSource (const std::string &source) {
	Lexer lexer(source);
	return Parser(lexer).parse();
}

const FunctionDeclaration &firstFunction (const SourceUnit &source) {
	return dynamic_cast<const FunctionDeclaration &>(*source.declarations.at(0));
}

TEST (ParserSemicolons, WrappedSubtractionContinuesTheInitializer) {
	const SourceUnit source = parseSource("pub fn<int> main() {\r\n    def n := 7\r\n        - 2;\r\n    return n;\r\n}");
	const auto      &body   = std::get<BlockBody>(firstFunction(source).body.value);
	ASSERT_EQ(body.statements.size(), 2u);
	const auto &binding = dynamic_cast<const BindingDeclaration &>(*body.statements.at(0));
	EXPECT_EQ(dynamic_cast<const BinaryExpression &>(*binding.initializer).op.getOperator(), OMinus);
}

TEST (ParserSemicolons, UnbracedConditionalLeavesTheNextArgumentToTheCall) {
	const SourceUnit source = parseSource("fn<int> pick(bool ready) = pair(if (ready) 4 else 2, 3);");
	const auto      &call   = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(firstFunction(source).body.value).expression);
	ASSERT_EQ(call.suppliedArguments.size(), 2u);
	const auto &conditional = dynamic_cast<const IfExpression &>(*call.suppliedArguments.front().expression);
	if (conditional.otherwise) {
		const auto &arm = dynamic_cast<const ExpressionStatement &>(*conditional.otherwise->at(0));
		EXPECT_EQ(dynamic_cast<const IntegerLiteral &>(*arm.expression).value, IntegerValue{ 2 });
	} else {
		FAIL() << "Expected an else arm on the first argument";
	}
	EXPECT_EQ(dynamic_cast<const IntegerLiteral &>(*call.suppliedArguments.at(1).expression).value, IntegerValue{ 3 });

	const SourceUnit results = parseSource("fn<int,int> choose(bool b) = if (b) { 4, 3; } else { 2, 1; };");
	const auto      &choice  = dynamic_cast<const IfExpression &>(*std::get<ExpressionBody>(firstFunction(results).body.value).expression);
	const auto      &tail    = dynamic_cast<const ExpressionStatement &>(*choice.branches.at(0).statements.at(0));
	EXPECT_EQ(dynamic_cast<const ResultListExpression &>(*tail.expression).values.size(), 2u);
}

TEST (ParserSemicolons, DeclarationAndMemberListsOwnTheirTerminators) {
	EXPECT_NO_THROW(parseSource(R"(using app
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
		EXPECT_THROW(parseSource(source), cde::SyntaxException);
	}
}

TEST (ParserSemicolons, PublicBlocksKeepChildSpansAndDeclarationBoundaries) {
	const SourceUnit source = parseSource("using helpers: *;\npub {\n  pub {}\n  fn first() {}\n  pub { pub fn second() {} }\n}\nfn last() {}");
	ASSERT_EQ(source.declarations.size(), 3u);
	const FunctionDeclaration &first = firstFunction(source);
	EXPECT_EQ(first.span.toString(), "4:3-4:16");
	ASSERT_TRUE(first.publicBlock);
	EXPECT_EQ(first.publicBlock->toString(), "2:1-2:4");
	const auto &second = dynamic_cast<const FunctionDeclaration &>(*source.declarations[1]);
	EXPECT_EQ(second.span.toString(), "5:9-5:27");
	if (!second.visibility) FAIL() << "Expected the child's written pub modifier";
	EXPECT_EQ(second.visibility->getSpan().toString(), "5:9-5:12");
	EXPECT_FALSE(source.declarations[2]->publicBlock);

	using Code = cde::DiagnosticCode;

	struct InvalidBlock {
		const char *source;
		Code        code;
		const char *span;
	};

	for (const auto &[input, code, span] : std::array{
	             InvalidBlock{ .source = "pub {}\nusing helpers: *;", .code = Code::ImportOutsideHeader, .span = "2:1-2:6" },
	             InvalidBlock{ .source = "pub {\n  using helpers: *;\n}", .code = Code::ImportOutsideHeader, .span = "2:3-2:8" },
	             InvalidBlock{ .source = "pub {\nfn f() {}\n", .code = Code::ExpectedToken, .span = "3:1-3:1" },
	             InvalidBlock{ .source = "pub {\nfn<int> f() = 1\n}", .code = Code::ExpectedSemicolon, .span = "2:16-2:16" },
	     }) {
		SCOPED_TRACE(input);
		try {
			parseSource(input);
			FAIL() << "Expected a syntax error";
		} catch (const cde::SyntaxException &error) {
			EXPECT_EQ(error.getCode(), code);
			ASSERT_FALSE(error.getSourceLabels().empty());
			EXPECT_EQ(error.getSourceLabels().front().span.toString(), span);
		}
	}
	for (const char *input :
	     { "fn f() { pub {} }", "typedef<class> C { mt f() { pub {} } }", "pub {};", "typedef<struct> S { pub { int x; } }",
	       "typedef<enum> E { pub { One; } }", "typedef<state<void>> S for C { pub {} }", "pub { pub typedef<state<void>> S for C; }" }) {
		SCOPED_TRACE(input);
		EXPECT_THROW(parseSource(input), cde::SyntaxException);
	}
}

TEST (ParserSemicolons, ControlFlowAndBraceInitializersOwnSeparateEndings) {
	const SourceUnit                     source = parseSource(R"(fn run(bool b) {
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

	EXPECT_NO_THROW(parseSource("fn stop(bool b) = if (b) { return; };"));
	EXPECT_NO_THROW(parseSource("fn<int> early(bool b) = if (b) return 7; else 2;"));
	for (const char *invalid : { "fn f() { ; }", "fn f() { act();; }", "fn f() { while (true); }", "fn f() { do {} while (true) }",
	                             "fn f() { int later := { return 42; } }", "fn f(bool b) = if (b) { return; }" }) {
		SCOPED_TRACE(invalid);
		EXPECT_THROW(parseSource(invalid), cde::SyntaxException);
	}
}

TEST (ParserSemicolons, HeaderOnlyTypesPreserveFieldsContractsAndStateAttachments) {
	for (const std::string ending : { ";", " {}" }) {
		SCOPED_TRACE(ending);
		const std::string source = "typedef<interface> Tag" + ending + " typedef<class> Ticket(pub def<int> number) : Tag" + ending +
		                           " typedef<struct> Marker" + ending + " pub typedef<class> Machine { pub default state<void> Idle" + ending +
		                           " pub state Ready; } typedef<state<void>> pub Ready for Machine" + ending +
		                           " fn<int> number(Ticket t) = t.number; fn<bool> entered(Machine m) { m -> Ready; return m is Ready; }";
		EXPECT_NO_THROW((CheckedProgram{ SymbolTable(parseSource(source)) }));
	}
	EXPECT_THROW((CheckedProgram{ SymbolTable(parseSource("typedef<class> Ticket; typedef<class> Ticket {}")) }), cde::ClothDiagnostic);
	EXPECT_THROW((CheckedProgram{ SymbolTable(parseSource("typedef<interface> I { pub mt<int> value(); } typedef<class> C : I;")) }), cde::ClothDiagnostic);
}

TEST (ParserSemicolons, UnterminatedClassHeaderAtEofHasAnInsertionPoint) {
	const std::string source = "pub typedef<class> MyClass";
	try {
		parseSource(source);
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

TEST (ParserSemicolons, TransitiveImportsAcceptExplicitTerminators) {
	const std::filesystem::path entry  = std::filesystem::path(__FILE__).parent_path().parent_path() / "Inputs" / "semicolon_entry.co";
	const std::string           source = "using semicolon_module: Ticket;\nfn accepts(Ticket item) {}\npub fn<int> main() = 42;";
	Lexer                       lexer(SourceFile(entry, source));
	const CheckedProgram        program{ SymbolTable(Parser(lexer).parse()) };
	EXPECT_EQ(program.units.size(), 3u);
}

TEST (ParserSemicolons, PhysicalWhitespacePreservesGenericAdjacencyAndNewlinesInsideLiterals) {
	const SourceUnit applied = parseSource("fn<T> f(T x) = first<T>(\r\nx\r\n);");
	const auto      &call    = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(firstFunction(applied).body.value).expression);
	EXPECT_TRUE(call.typeArguments);
	for (const char *expression : { "first <T>(x)", "first\n<T>(x)", "first<T> (x)", "first<T>\r\n(x)" }) {
		SCOPED_TRACE(expression);
		const SourceUnit comparison = parseSource(std::string("fn<T> f(T x) = ") + expression + ";");
		EXPECT_NE(dynamic_cast<const BinaryExpression *>(std::get<ExpressionBody>(firstFunction(comparison).body.value).expression.get()), nullptr);
	}
	const SourceUnit literal = parseSource("fn<string> text() = \"\"\"first\nsecond\"\"\";");
	EXPECT_EQ(dynamic_cast<const StringLiteral &>(*std::get<ExpressionBody>(firstFunction(literal).body.value).expression).value, "first\nsecond");
}

TEST (ParserSemicolons, WrappedReturnCallsAndIndexingKeepTheirValues) {
	const SourceUnit source = parseSource("fn<int, int> pick(Things\n items) { return // keep reading\r\n"
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
		parseSource(source);
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
