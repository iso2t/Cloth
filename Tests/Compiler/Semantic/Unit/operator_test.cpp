// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/expr/expression.hpp"
#include "ast/expr/expressions.hpp"
#include "cde/diagnostic_code.hpp"
#include "checked_program_test.hpp"
#include "gtest/gtest.h"
#include "semantic/checked_program.hpp"
#include "semantic/symbol_table.hpp"
#include "token/token.hpp"

namespace cloth::test {
namespace {

using Code = cde::DiagnosticCode;

const ExpressionInfo &resultOf (const CheckedProgram &program, const char *function) {
	return program.expressions.at(&bodyExpression(program.units.front()->getGlobals().at(function).front()));
}

TEST (SemanticOperators, ChecksContractsBeforeUse) {
	for (const char *s_member : { "operator *(int a, int b) = a;", "operator -(opt<int> x ?: 1) = x;", "operator !(int x) = true;", "operator +(self) = 1;",
	                              "operator<int> +(int x) = x;", "operator<unsigned int<auto>> +() = 1;", "operator ++(int x) {}",
	                              "operator +=(int x, self) {}", "operator +=(int x) -> int = x;", "operator <(int x) -> int = x;",
	                              "operator +(int x) -> void {}", "operator +(int x) = null;", "operator ::label(int x) = x;", "operator ,(int x) = x;" }) {
		expectError(std::string("typedef<class> C { ") + s_member + " }", Code::InvalidOperatorDeclaration);
	}
}

TEST (SemanticOperators, CapturedWidthsSelectAndRetainIndependentOverloads) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<class> Gauge {
    mt<int<8>> pick(int<8> x) = x;
    mt<T> pick<T>(T x) = x;
    pub operator<int<auto>> +() = pick(1 -> int<auto>);
}
fn<int<8>> narrow(Gauge g, int<8> n) = g + n;
fn<int<16>> wide(Gauge g, int<16> n) = n + g;
fn<int<8>> again(Gauge g, int<8> n) = n + g;
)");
	const ExpressionInfo &ei_narrow = resultOf(cp_program, "narrow"), &ei_wide = resultOf(cp_program, "wide"), &ei_again = resultOf(cp_program, "again");
	ASSERT_TRUE(ei_narrow.leftContext && ei_wide.rightContext && ei_again.rightContext);
	const GenericInstance *gi_narrow = ei_narrow.leftContext->instance, *gi_wide = ei_wide.rightContext->instance;
	ASSERT_NE(gi_narrow, nullptr);
	ASSERT_NE(gi_wide, nullptr);
	EXPECT_NE(gi_narrow, gi_wide);
	EXPECT_EQ(gi_narrow, ei_again.rightContext->instance);
	EXPECT_EQ(gi_narrow->signature.returns.front().width, 8u);
	EXPECT_EQ(gi_wide->signature.returns.front().width, 16u);
	const Symbol     &sym_gauge = cp_program.units.front()->getGlobals().at("Gauge").front();
	const Symbol     &sym_op    = sym_gauge.members.at("+").front();
	const Expression &e_pick    = bodyExpression(sym_op);
	EXPECT_FALSE(cp_program.expressions.contains(&e_pick));
	EXPECT_EQ(gi_narrow->body.expressions.at(&e_pick).symbol, &sym_gauge.members.at("pick").front());
	EXPECT_EQ(gi_wide->body.expressions.at(&e_pick).symbol, &sym_gauge.members.at("pick").back());
}

TEST (SemanticOperators, ChecksWidthTemplateNamesAndOnlyTypesSelectedBodies) {
	const std::string s_gauge = R"(typedef<class> Item { pub mt read() {} }
typedef<struct> Gauge { Item backing;
    mt<Item> make(int<8> n) = backing;
    mt<Item?> make<T>(T n) = null;
    pub operator<int<auto>> +() -> int<auto> {
        Item? result = make(1 -> int<auto>); result.read(); return 1;
    }
}
)";
	EXPECT_NO_THROW(checkSource(s_gauge));
	EXPECT_NO_THROW(checkSource(s_gauge + "fn use(Gauge g, int<8> n) { g + n; }"));
	expectError(s_gauge + "fn use(Gauge g, int<8> n, int<16> m) { g + n; g + m; }", Code::NullablePayloadAccess);
	expectError(R"(typedef<class> Gauge { pub operator<int<auto>> +() -> int<auto> { int n = "x"; return missing; } })", Code::UnknownName);
	expectError("typedef<class> Gauge<T> { pub operator<int<auto>> +() -> int<auto> = false; }", Code::ReturnType);
	const std::string s_annotation = "typedef<class> Gauge { pub operator<int<auto>> +() -> int<auto> { Missing item = 1; return 1; } }\n";
	EXPECT_NO_THROW(checkSource(s_annotation));
	expectError(s_annotation + "fn use(Gauge g) { g + 1; }", Code::UnsupportedType);
	expectError(s_annotation + "fn generic<T>() {}", Code::UnsupportedType);
}

TEST (SemanticOperators, ResolvesWidthDependentMethodArgumentsAtTheChosenWidth) {
	const std::string s_global = "fn<T> echo<T>(T x) = x;\n";
	const std::string s_prefix = "typedef<class> Gauge { mt<T> pick<T>(T x) = x;\n pub operator<int<auto>> +() -> int<auto> { ";
	const std::string s_use    = "; } }\nfn<int<8>> narrow(Gauge g, int<8> n) = g+n;";
	EXPECT_NO_THROW(checkSource(s_prefix + "int<auto>[] items = [1]; return pick<int<auto>>(items[0])" + s_use));
	expectError(s_global + s_prefix + "return echo<int<auto>>(1)" + s_use, Code::UnsupportedType);
	EXPECT_NO_THROW(checkSource("fn<T> echo<T,U=bool>(T x) = x;\n" + s_prefix + "return echo<int<auto>>(1)" + s_use));
	expectError("typedef<class> Box<T> {}\n" + s_prefix + "Box<int<auto>>? b=null; return 1" + s_use, Code::UnsupportedType);
}

TEST (SemanticOperators, KeysCapturedWidthsByClosedReceiver) {
	const CheckedProgram   cp_program = checkSource(R"(typedef<class> Gauge<T>(def<T> value) {
    pub operator<float<auto>> +() -> float<auto> { T local = value; return 1.0; }
}
fn<float<16>> first(Gauge<bool> g, float<16> n) = g + n;
fn<float<16>> second(Gauge<string> g, float<16> n) = g + n;
)");
	const GenericInstance *gi_first   = resultOf(cp_program, "first").leftContext.value().instance;
	const GenericInstance *gi_second  = resultOf(cp_program, "second").leftContext.value().instance;
	ASSERT_NE(gi_first, nullptr);
	ASSERT_NE(gi_second, nullptr);
	EXPECT_NE(gi_first, gi_second);
	ASSERT_EQ(gi_first->body.localTypes.size(), 1u);
	EXPECT_EQ(gi_first->body.localTypes.begin()->second, ResolvedType(KBoolean));
	EXPECT_EQ(gi_second->body.localTypes.at(gi_first->body.localTypes.begin()->first), ResolvedType(KString));
}

TEST (SemanticOperators, ResolvesConcreteOverridesAndKeepsBuiltInFallbacks) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<class> Item {
    pub operator ==(Item other) = true;
    pub operator ==(Object other) = false;
    pub operator ===(int marker, self) = false;
    pub operator &&(bool flag) = flag;
    pub operator ?:(int fallback) = fallback;
    pub operator<int> ->() = 1;
    pub operator<int> is() = false;
}
typedef<class> Tag { pub operator ==(string other) = false; }
fn<bool> exact(Item a, Item b) = a == b;
fn<bool> lifted(Item? a, Item b) = a != b;
fn<bool> root(Item a, Object b) = a == b;
fn<bool> fallback(Tag a, Item b) = a == b;
fn<bool> reverse(Item a) = 3 === a;
fn<bool> logical(Item a) = a && false;
fn<int> coalesce(Item a) = a ?: 3;
fn<int> cast(Item a) = a -> int;
fn<bool> matches(Item a) = a is int;
fn<bool> raw(Object a, Tag b) = a === b;
)");
	const ExpressionInfo &ei_exact   = resultOf(cp_program, "exact");
	ASSERT_NE(ei_exact.symbol, nullptr);
	EXPECT_EQ(cp_program.declarations.at(ei_exact.symbol).parameters.front().name, "Item");
	EXPECT_EQ(resultOf(cp_program, "lifted").symbol, ei_exact.symbol);
	EXPECT_TRUE(resultOf(cp_program, "lifted").negateEquality);
	EXPECT_TRUE(resultOf(cp_program, "lifted").liftedEquality);
	ASSERT_NE(resultOf(cp_program, "root").symbol, nullptr);
	EXPECT_NE(resultOf(cp_program, "root").symbol, ei_exact.symbol);
	for (const char *s_name : { "reverse", "logical", "coalesce", "cast", "matches" })
		EXPECT_NE(resultOf(cp_program, s_name).argumentContract, nullptr) << s_name;
	EXPECT_EQ(resultOf(cp_program, "raw").argumentContract, nullptr);
	EXPECT_EQ(resultOf(cp_program, "fallback").argumentContract, nullptr);
	expectError("typedef<class> P { operator ==(P rhs) = true; }\nfn<bool> f(P a) = a == a;", Code::PrivateClassMember);
	expectError("typedef<class> A { pub operator +(int v) = v; }\nfn<int> f(A a) = a + \"bad\";", Code::OperatorMatch);
}

TEST (SemanticOperators, ComparesArraysByShapeAndElementEquality) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<struct> P<T> { pub operator ==(P<T> other, self) = true; }
typedef<enum> E { A, B; }
fn<bool> compared(P<int>?[2][]? a, P<int>?[3][] b) = a != b;
fn<bool> primitive(int[2] a, int[3] b) = a == b;
fn<bool> enumeration(E[] a, E[] b) = a != b;
fn<bool> literal(int<8>[1] a) = [1, 2] == a;
fn<bool> empty(int[2][1] a) = [] != a;
fn<bool> narrow(int<8> n) = [n] == [1];
fn<bool> narrowOther(int<8> n) = [1] == [n];
fn<bool> absent(int[]? a) = a == null;
)");
	const ExpressionInfo &ei_array   = resultOf(cp_program, "compared");
	ASSERT_TRUE(ei_array.elementEquality);
	EXPECT_NE(ei_array.elementEquality->instance, nullptr);
	EXPECT_EQ(ei_array.elementEquality->symbol->name.getOperator(), OEqual);
	EXPECT_TRUE(ei_array.callsElementEquality);
	EXPECT_FALSE(resultOf(cp_program, "primitive").callsElementEquality);
	EXPECT_FALSE(resultOf(cp_program, "enumeration").elementEquality);
	const BinaryExpression &be_empty = dynamic_cast<const BinaryExpression &>(bodyExpression(cp_program.units.front()->getGlobals().at("empty").front()));
	EXPECT_EQ(cp_program.expressions.at(be_empty.left.get()).types.front().element().suffixes.back(), "[1]");
	for (const char *s_source :
	     { "fn<bool> f(int[] a) = a === a;", "fn<bool> f(int a) = a === a;", "fn<bool> f(int?[] a, int[] b) = a == b;",
	       "fn<bool> f(int[][] a, int[] b) = a != b;", "typedef<struct> P {}\nfn<bool> f(P a) = a == a;", "typedef<struct> P {}\nfn<bool> f(P[] a) = a == [];",
	       "typedef<struct> P { pub operator !=(P p) = true; }\nfn<bool> f(P[] a) = a != a;" })
		expectError(s_source, Code::IncomparableTypes);
	expectError("fn<bool> f(int[] a) = a == null;", Code::InvalidNullComparison);
	expectError("typedef<struct> P { pub operator ==(P p) = true;\npub operator ==(P p, self) = false; }\nfn<bool> f(P[] a) = a == a;", Code::OperatorMatch);
	expectError("typedef<struct> P { operator ==(P p) = true; }\nfn<bool> f(P[] a) = a != a;", Code::PrivateClassMember);
}

TEST (SemanticOperators, AppliesOperandContextsInSourceOrder) {
	const CheckedProgram cp_program = checkSource(R"(typedef<struct> A { pub operator<B> ==() = "x"; }
typedef<struct> B { pub operator<A> ==() = "x"; }
typedef<struct> Tag { pub operator<string> ==() = "x"; }
fn<bool> both(A a, B b) = a != b;
fn<bool> right(string text, Tag tag) = text == tag;
)");
	EXPECT_TRUE(resultOf(cp_program, "both").leftContext);
	EXPECT_TRUE(resultOf(cp_program, "both").rightContext);
	EXPECT_FALSE(resultOf(cp_program, "right").leftContext);
	EXPECT_TRUE(resultOf(cp_program, "right").rightContext);
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Word { pub operator<string> ==() = "x"; }
fn<bool> f(Word w) { string rhs = { w::delete; return "x"; }; return w == rhs; }
)"));
	expectError(R"(typedef<class> Box { pub def<string?> name = "x"; }
typedef<class> Clear { pub operator<usize> ==() -> usize = 0; }
fn<bool> f(Box b, Clear c) { if (b.name != null) return c == b.name::size; return false; }
)",
	            Code::NullablePayloadAccess);
}

TEST (SemanticOperators, ShortCircuitsOnlyTheBuiltInLogicalOperations) {
	const std::string s_ops = R"(typedef<class> P { pub operator &&(bool flag, self) = flag;
pub operator ||(bool flag, self) = flag; }
)";
	for (const char *s_expr : { "false && p", "true || p" })
		expectError(s_ops + "fn<bool> f(P p) { p::delete; return " + s_expr + "; }", Code::DeletedClassAccess);
	EXPECT_NO_THROW(checkSource(s_ops + "fn<bool> use(P p) { p::delete; return true || (true && p); }"));
	expectError("typedef<class> P { pub operator &&(bool b) = b; }\nfn f(P p, opt<bool> x ?: p && true) {}", Code::InvalidParameterDefault);
}

TEST (SemanticOperators, LogicalOperandContextsFollowTheEvaluatedPath) {
	const std::string ops = R"(typedef<class> Flag { pub operator<bool> &&() = true; pub operator<bool> ||() = false; }
typedef<class> Box(pub def<string?> name, pub def<Flag> flag) {}
)";
	for (const char *expr : { "false && p", "true || p" }) EXPECT_NO_THROW(checkSource(ops + "fn<bool> f(Flag p) { p::delete; return " + expr + "; }"));
	for (const char *expr : { "true && p", "false || p", "p && false", "p || true" })
		expectError(ops + "fn<bool> f(Flag p) { p::delete; return " + expr + "; }", Code::DeletedClassAccess);
	EXPECT_NO_THROW(checkSource(ops + R"(
fn<bool> guardedAnd(Box? box) = box != null && box.flag;
fn<bool> guardedOr(Box? box) = box == null || box.flag;
fn<string> present(Flag flag, Box box) {
    if (flag && box.name != null) return box.name;
    if (flag || box.name == null) return "";
    return box.name;
}
fn<string> skipped(Flag flag, Box box) {
    if (box.name == null) return "";
    bool skipped = false && flag;
    return box.name;
}
)"));
	expectError(ops + R"(fn<usize> cleared(Flag flag, Box box) {
    if (box.name == null) return 0;
    bool taken = true && flag;
    return box.name::size;
})",
	            Code::NullablePayloadAccess);
}

TEST (SemanticOperators, UpdatesCapturedClassElementsAndFallsBackToArithmetic) {
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Item {
    pub operator ++() {}
    pub operator +=(string text) {}
    pub operator +(int n) -> Item = self;
}
typedef<class> Items(def<Item> item) {
    pub operator [](int n) = item;
    operator []?(int n) = true;
    pub operator []=(int n, Item value) { item = value; }
}
fn update(Items items, Item item) { items[0]++; items[1] += "x"; item += 1; }
)"));
	expectError(
	        "typedef<class> Item { pub operator ++() {} }\ntypedef<class> Items(def<Item> item) { pub operator [](int n) = item; }\nfn f(Items items) { items[0]++; }",
	        Code::InvalidIndex);
	expectError(
	        "typedef<class> Item { pub operator ++() {} }\ntypedef<class> Items(def<Item> item) { pub operator [](int n) = item;\noperator []?(int n) = true;\npub operator []=(int n, Item value) {} }\n"
	        "fn f(Items items) { items::delete; items[0]++; }",
	        Code::DeletedClassAccess);
}
}
}
