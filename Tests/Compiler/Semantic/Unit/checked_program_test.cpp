// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>

#include <cde/cloth_diagnostic.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>

namespace cloth {
namespace {

CheckedProgram checkSource (const std::string &source) {
	Lexer lexer(source);
	return CheckedProgram(SymbolTable(Parser(lexer).parse()));
}

void expectError (const std::string &source, cde::DiagnosticCode code) {
	SCOPED_TRACE(source);
	try {
		checkSource(source);
		FAIL() << "Expected a semantic diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), code) << error.what();
		ASSERT_FALSE(error.get_source_labels().empty());
		EXPECT_NE(error.get_source_labels().front().span.source, nullptr);
	}
}

TEST (SemanticChecking, RetainsSelectedOverloadAndLexicalBinding) {
	const CheckedProgram     program = checkSource(R"(fn<int> entry(int x) = choose(second = x, first = 1)
fn<bool> choose(bool first, bool second) = first
fn<int> choose(int first, int second) = entry(second)
)");
	const SymbolScope       &globals = program.units.front()->get_globals();
	const CallableSignature &entry   = *std::get<const CallableSignature *>(globals.at("entry")[0].declaration);
	const CallExpression    &call    = dynamic_cast<const CallExpression &>(*std::get<ExpressionBody>(entry.executableBody()->value).expression);
	const ExpressionInfo    &result  = program.expressions.at(&call);
	EXPECT_EQ(result.symbol, &globals.at("choose")[1]);
	EXPECT_EQ(result.types, ResultTypes{ ResolvedType(kInteger) });
	EXPECT_EQ(result.parameterForArgument, (std::vector<std::size_t>{ 1, 0 }));
	EXPECT_EQ(program.expressions.at(call.suppliedArguments[0].expression.get()).binding, &entry.parameters[0].name);
}

TEST (SemanticChecking, PreservesCompoundTypeSpellingsDuringResolution) {
	const CheckedProgram   cp_program = checkSource(R"(fn<unsigned int<8>> first(unsigned int<8>?[] values) = values[0] ?: 0
fn<unsigned int<8>> use() = first([42 -> unsigned int<8>, null])
)");
	const Symbol          &sym_first  = cp_program.units.front()->get_globals().at("first").front();
	const DeclarationInfo &di_first   = cp_program.declarations.at(&sym_first);
	ASSERT_EQ(di_first.parameters.size(), 1u);
	ASSERT_EQ(di_first.returns.size(), 1u);
	EXPECT_EQ(di_first.parameters.front().spelling(), "unsigned int<8>?[]");
	EXPECT_EQ(di_first.returns.front().spelling(), "unsigned int<8>");
}

TEST (SemanticChecking, MatchesExactContextualOptionalAndGenericArguments) {
	EXPECT_NO_THROW(checkSource(R"(fn<int> take(int x) = x
fn<bool> take(int<8> x) = true
fn<int> withDefault(opt<int> step ?: 1, int x) = step + x
fn<T> first<T>(T[] values) = values[0]
fn<int> entry() = withDefault(take(first([42])))
fn<bool> narrow() = take(1 -> int<8>)
)"));
	using Code = cde::DiagnosticCode;
	expectError("fn f(int x) {}\nfn<int> f(int y) = 1", Code::DUPLICATE_FUNCTION);
	expectError("fn f(int<8> x) {}\nfn f(int<16> x) {}\nfn run() = f(1)", Code::AMBIGUOUS_CALL);
	expectError("fn f(int x) {}\nfn run() = f(true)", Code::NO_MATCHING_CALL);
	expectError("fn f(int x) {}\nfn run() = f()", Code::CALL_ARITY);
	expectError("fn f(int x,int y) {}\nfn run() = f(y=1,2)", Code::INVALID_ARGUMENT_BINDING);
	expectError("fn<int> run() = missing()", Code::UNKNOWN_NAME);
}

TEST (SemanticChecking, ChecksBodiesScopesAssignmentsAndAllReturnPaths) {
	using Code = cde::DiagnosticCode;
	EXPECT_NO_THROW(checkSource(R"(fn<int,int> pair(int x) = x,x
fn<int> run(int n) {
    def<int,int> a,b = pair(n)
    if (a > 0) { return b } else { return a }
}
)"));
	expectError("fn<int> run(bool condition) { if (condition) { return 1 } }", Code::MISSING_RETURN);
	expectError("fn<int> run() = true", Code::RETURN_TYPE);
	expectError("fn run() { def local = later\ndef later = 1 }", Code::UNKNOWN_NAME);
	expectError("fn run() { if (true) { def x = 1 }\ndef y = x }", Code::UNKNOWN_NAME);
	expectError("fn run(final int x) { x = 2 }", Code::IMMUTABLE_BINDING);
	expectError("fn run() { def x := 1\nx++ }", Code::IMMUTABLE_BINDING);
	expectError("fn run() { def<int> x = true }", Code::TYPE_MISMATCH);
	expectError("fn run() { if (1) {} }", Code::NON_BOOLEAN_CONDITION);
	expectError("fn run() { break }", Code::INVALID_LOOP_JUMP);
	expectError("fn<Missing> run() = 1", Code::UNSUPPORTED_TYPE);
}

TEST (SemanticChecking, EnforcesMemberVisibilityAndSpecializesReceiverTypes) {
	EXPECT_NO_THROW(checkSource(R"(pub typedef<class> Box<T>(def<T> item) {
    pub mt<T> get() = self.item
    mt<T> other(Box<T> box) = box.item
}
fn<int> use(Box<int> box) = box.get()
)"));
	using Code = cde::DiagnosticCode;
	expectError("pub typedef<struct> S { int x }\nfn<int> leak(S s) = s.x", Code::PRIVATE_CLASS_MEMBER);
	expectError("typedef<class> C { mt<int> read() = 1 }\nfn<int> leak(C c) = c.read()", Code::PRIVATE_CLASS_MEMBER);
	expectError("pub typedef<enum> E { pub A, B }\nfn<E> leak() = E.B", Code::PRIVATE_CLASS_MEMBER);
	expectError("pub typedef<class> C { typedef<struct> Hidden {} }\nfn use(C.Hidden h) {}", Code::PRIVATE_NESTED_TYPE);
	expectError("typedef<class> C { pub mt<int> pick(bool x) = 1\nmt<int> pick(int x) = x }\nfn<int> leak(C c) = c.pick(2)", Code::PRIVATE_CLASS_MEMBER);
}

TEST (SemanticChecking, ChecksFieldWritesNullableGuardsLoopsAndMultiResultAssignments) {
	EXPECT_NO_THROW(checkSource(R"(typedef<struct> Point { pub int x }
fn<int,int> pair(int n) = n, n
fn<int> sum(Point? point, int[] items) {
    if (point == null) return 0
    def total = point.x
    for (def n <- items) total += n
    def a,b = pair(total)
    a,b = pair(b)
    while (a > 0) { a-- }
    return b
}
)"));
	using Code = cde::DiagnosticCode;
	expectError("fn run(final int[] items) { items[0] = 1 }", Code::IMMUTABLE_BINDING);
	expectError("typedef<enum> E { pub First }\nfn run() { E.First = E.First }", Code::INVALID_FIELD_TARGET);
	expectError("fn<int> bad(int? n) { if (n != null) { n = null\nreturn n }\nreturn 0 }", Code::RETURN_TYPE);
	expectError("fn<int,int> pair() = 1, 2\nfn run() { def a,b,c = pair() }", Code::RECEPTION_ARITY);
}

TEST (SemanticChecking, ChecksInheritedOverloadsAndGenericBounds) {
	EXPECT_NO_THROW(checkSource(R"(typedef<interface> Readable { pub mt<int> read() }
typedef<class> Base : Readable {
    virtual: pub mt<int> read() = 1
    pub mt<int> read(int x) = x
}
typedef<class> Derived : Base { override: pub mt<int> read() = 2 }
fn<int> through<T: Readable>(T x) = x.read()
fn<int> check(Derived d) = through(d) + d.read(3) + d.read()
)"));
	using Code = cde::DiagnosticCode;
	expectError("fn<T> choose<T>(T x) = x\nfn<U> choose<U>(U y) = y", Code::DUPLICATE_FUNCTION);
	expectError("typedef<interface> I {}\nfn<T> constrained<T:I>(T x) = x\nfn<int> wrong() = constrained(1)", Code::NO_MATCHING_CALL);
	expectError("typedef<class> A : B\ntypedef<class> B : A", Code::INTERFACE_INHERITANCE_CYCLE);
	expectError("typedef<interface> I { pub mt<int> get() }\ntypedef<class> C : I", Code::MISSING_INTERFACE_IMPLEMENTATION);
	expectError("typedef<interface> I { pub mt<int> get() }\ntypedef<class> C : I { mt<int> get() = 0 }", Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
	expectError("typedef<interface> I { pub mt<int> get() }\ntypedef<class> C : I { pub mt<bool> get() = true }", Code::INCOMPATIBLE_INTERFACE_REQUIREMENT);
}

TEST (SemanticChecking, ChecksOperatorsConstructorsAndStatePayloads) {
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Counter(def<int> n) {
    pub Counter() : (0) {}
    pub operator +(int delta) = self.n + delta
    pub operator []=(int i, int value) { self.n = value }
    pub state On
    pub state<void> ::clear()
}
typedef<state<int>> On for Counter { pub mt clear() { self -> On(1) } }
fn<int> work() {
    def c = Counter::new()
    c[0] = 3
    c -> On(2)
    c::clear()
    return c + 2
}
)"));
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> Box(int x)\nfn<Box> leak() = Box::new(1)", Code::PRIVATE_CLASS_MEMBER);
	expectError("typedef<class> Lamp { state Missing }", Code::INVALID_STATE_ATTACHMENT);
	expectError("typedef<class> C { pub state<int> On {} }\nfn bad(C c) { c -> On() }", Code::INVALID_STATE_PAYLOAD);
}

TEST (SemanticChecking, ResolvesUpdatesAndDistinguishesStateTypeAndValueTests) {
	const CheckedProgram checked = checkSource(R"(typedef<class> Meter(def<int> n) {
    pub operator +=(int amount) { self.n += amount }
    pub operator ++() { self.n++ }
    pub state<void> Ready {}
}
fn<bool> test(Meter m) {
    m += 3
    m++
    m -> Ready
    def range = 1 .. 3
    return m is Ready && m is Meter && 2 is range
}
)");
	EXPECT_EQ(checked.writes.size(), 2u);
	EXPECT_EQ(checked.valueTests.size(), 1u);
	using Code = cde::DiagnosticCode;
	expectError("typedef<class> C { state<void> Hidden {} }\nfn<bool> bad(C c) = c is Hidden", Code::PRIVATE_CLASS_MEMBER);
	expectError("fn<bool> bad(int n) = 3 is n", Code::INVALID_RANGE_MEMBERSHIP);
	expectError("fn<int> bad(int? n) = n -> int", Code::NULLABLE_PAYLOAD_ACCESS);
}

TEST (SemanticChecking, InfersClassArgumentsAndResolvesBoundConstructors) {
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Cell<T>(def<T> item, int unused) {
    pub Cell(T value) : (value, 0) {}
}
fn<T> construct<T: new(int value)>(int value) = T::new(value)
fn<Cell<int>> inferred() = Cell::new(5)
fn<Cell<int>> factory() = construct<Cell<int>>(5)
)"));
	expectError("typedef<class> Hidden(int value)\nfn<T> make<T: new(int value)>() = T::new(1)\nfn<Hidden> run() = make<Hidden>()",
	            cde::DiagnosticCode::NO_MATCHING_CALL);
}

TEST (SemanticChecking, ChecksGroupedBindingsAndTheirDeclaredTypes) {
	EXPECT_NO_THROW(checkSource(R"(fn<int> private() {
    def<int<8>> a,b = 1
    a,b = 2
    def x, y = a, b
    while (true) {
        return (x + y) -> int
        break
    }
}
)"));
	expectError("fn run() { def<int,int> a,b = 1 }", cde::DiagnosticCode::RECEPTION_ANNOTATIONS);
	expectError("fn run() { def<int> a,b = 1, 2 }", cde::DiagnosticCode::RECEPTION_ANNOTATIONS);
}

}
}
