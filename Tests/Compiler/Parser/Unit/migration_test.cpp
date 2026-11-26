// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>
#include <string>
#include <utility>

#include <ast/source_unit.hpp>
#include <lexer/lexer.hpp>
#include <migration/structure.hpp>
#include <parser/parser.hpp>

namespace cloth::migration {
namespace {

SourceUnit parseTree (const std::string &text, const SourceGrammar grammar = SourceGrammar::ExplicitSemicolons) {
	Lexer lexer(text);
	return Parser(lexer, grammar).parse();
}

TEST (ParserMigration, ComparesStructureIndependentlyOfTheRewrite) {
	const std::pair<const char *, const char *> changes[] = {
		{ "using P: A, B;", "using P: B, A;" },
		{ "typedef<struct> S { int x; }", "typedef<struct> S { unsigned int x; }" },
		{ "typedef<class> C { pub def<int> x = 1; }", "typedef<class> C { def<int> x = 1; }" },
		{ "fn<int> f(opt<int> n ?: 1) = n;", "fn<int> f(opt<int> n ?: 2) = n;" },
		{ "fn f() { a(); b(); }", "fn f() { b(); a(); }" },
		{ "fn<int> f() = (1 + 2) * 3;", "fn<int> f() = 1 + 2 * 3;" },
		{ "fn<int> f() = g(x = 1);", "fn<int> f() = g(y = 1);" },
		{ "fn<int> f() = g<int>(1);", "fn<int> f() = g<uint>(1);" },
		{ "fn<int> f() = if (a) { 1, 2; } else 3;", "fn<int> f() = if (a) 1 else { 2, 3; };" },
		{ "fn f() { Box x := {1}; }", "fn f() { Box x := {1;}; }" },
		{ "fn f() { for (int i <- 0..3; i++) work(); }", "fn f() { for (int i <- 0..3; i--) work(); }" },
	};
	for (const auto &[left, right] : changes) {
		SCOPED_TRACE(left);
		EXPECT_TRUE(firstParseDifference(parseTree(left), parseTree(right)).has_value());
	}
	const auto legacy = parseTree("typedef<class> C {}\nfn<int,int> f() = if (a) 1, 2 else 3, 4", SourceGrammar::Legacy);
	const auto strict = parseTree("typedef<class> C;\r\nfn<int,int> f() = if (a) {1, 2;} else { 3, 4; };");
	EXPECT_FALSE(firstParseDifference(legacy, strict).has_value());
}

}
}
