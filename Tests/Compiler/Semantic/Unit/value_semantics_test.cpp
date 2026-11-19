// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cmath>

#include <ast/decl/enum_declaration.hpp>
#include <ast/stmt/for_statement.hpp>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {
using Code = cde::DiagnosticCode;

const ExpressionInfo &returned (const CheckedProgram &program, const std::string &name = "value") {
	return program.expressions.at(&bodyExpression(program.units.front()->get_globals().at(name).front()));
}

TEST (SemanticValues, TargetWidthsApplyToLiteralsCastsAndExtents) {
	for (unsigned bits : { 32u, 64u }) {
		const CheckedProgram program = checkSource("fn<usize> value() = usize::max\nfn<isize> low() = isize::min", { bits });
		EXPECT_EQ(std::get<IntegerConstant>(returned(program).numericConstant).magnitude, IntegerValue(bits / 32, UINT32_MAX));
		EXPECT_TRUE(std::get<IntegerConstant>(returned(program, "low").numericConstant).negative);
	}
	for (const std::string source : { "fn<usize> value() = 4294967296", "fn<isize> value() = -2147483649", "fn read(int[4294967296] data) {}" }) {
		SCOPED_TRACE(source);
		EXPECT_NO_THROW(checkSource(source, { 64 }));
		try {
			checkSource(source, { 32 });
			FAIL();
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.get_code(), source.starts_with("fn read") ? Code::INVALID_ARRAY : Code::INTEGER_RANGE);
		}
	}
}

TEST (SemanticValues, NumericConstantsPreserveWidthRoundingAndInvalidCasts) {
	const CheckedProgram program = checkSource(R"(fn<int<16>> value() = -1 -> unsigned int<8> -> int<16>
fn<unsigned int<65>> wide() = -1 -> unsigned int<64> -> float<64> -> unsigned int<65>
fn<float<16>> midpoint() = 1.00048828125
fn<float<16>> above() = 1.000488281250000001
fn<float<64>> negativeZero() = -0.0 -> float<64>
fn<int<8>> invalid() = 127.99 -> float<16> -> int<8>
fn<int> dynamic(float input) = input -> int
fn<int<8>?> nullable() = 127.5 -> float? -> int<8>?
fn<int<129>> divide() = ((18446744073709551616 + (0 -> int<129>)) * 9) / 3
fn<unsigned int<128>> quotient() = unsigned int<128>::max / 18446744073709551617
fn<unsigned int<128>> remainder() = unsigned int<128>::max % 18446744073709551617
fn<int> infinity() = (1.0 / 0.0) -> int
fn<int> nan() = (0.0 / 0.0) -> int
)");
	EXPECT_EQ(std::get<IntegerConstant>(returned(program).numericConstant), (IntegerConstant{ { 255 } }));
	EXPECT_EQ(std::get<IntegerConstant>(returned(program, "wide").numericConstant).magnitude, (IntegerValue{ 0, 0, 1 }));
	EXPECT_EQ(std::get<IntegerConstant>(returned(program, "divide").numericConstant).magnitude, (IntegerValue{ 0, 0, 3 }));
	EXPECT_EQ(std::get<IntegerConstant>(returned(program, "quotient").numericConstant).magnitude, (IntegerValue{ UINT32_MAX, UINT32_MAX }));
	EXPECT_EQ(std::get<IntegerConstant>(returned(program, "remainder").numericConstant).magnitude, (IntegerValue{ 0 }));
	EXPECT_TRUE(returned(program, "nan").invalidNumericCast);
	EXPECT_TRUE(returned(program, "infinity").invalidNumericCast);
	EXPECT_EQ(std::get<double>(returned(program, "midpoint").numericConstant), 1.);
	EXPECT_EQ(std::get<double>(returned(program, "above").numericConstant), 1.0009765625);
	EXPECT_TRUE(std::signbit(std::get<double>(returned(program, "negativeZero").numericConstant)));
	EXPECT_TRUE(returned(program, "invalid").invalidNumericCast);
	EXPECT_FALSE(returned(program, "dynamic").invalidNumericCast);
	EXPECT_EQ(std::get<IntegerConstant>(returned(program, "nullable").numericConstant), (IntegerConstant{ { 127 } }));
	expectError("fn<int> value(int x) = x / 0", Code::INTEGER_ZERO_DIVISOR);
	expectError("fn<int<8>> value() = int<8>::min % -1", Code::INTEGER_DIVISION_OVERFLOW);
}

TEST (SemanticValues, EnumConstantsAreCheckedInSourceOrderAndDefaultsAreRetained) {
	const CheckedProgram                   program = checkSource(R"(typedef<enum> E (pub def<int<8>> code = 7, pub def<int?[2]> row = [null, 1]) {
    ZED(3), ALPHA
})");
	const Symbol                          &owner   = program.units.front()->get_globals().at("E").front();
	const EnumDeclaration                 *decl    = static_cast<const EnumDeclaration *>(std::get<const TypeDeclaration *>(owner.declaration));
	const std::vector<const Expression *> &zed     = program.enumPayloads.at(&owner.members.at("ZED").front());
	ASSERT_EQ(zed.size(), 2u);
	EXPECT_EQ(zed[0], decl->cases.front().values.front().get());
	EXPECT_EQ(zed[1], decl->fields[1].defaultValue.get());
	expectError("fn<int> other() = 1\ntypedef<enum> E (def<int> n) { A(other()) }", Code::INVALID_ENUM_MEMBER);
	expectError("typedef<enum> E (def<int> n) { A(1 + 2) }", Code::INVALID_ENUM_MEMBER);
	expectError("typedef<enum> E (def<int<8>> n) { A(127.99 -> float<16> -> int<8>) }", Code::INVALID_CAST);
	expectError("typedef<enum> E (def<int<8>> n) { A(128) }", Code::INTEGER_RANGE);
	expectError("typedef<enum> E (def<int[2]> n) { A([1]) }", Code::INVALID_ARRAY);
}

TEST (SemanticValues, ArrayBindingsKeepInnerExtentsAndSeparateCountsFromDeclaredTypes) {
	const CheckedProgram program = checkSource(R"(fn run() {
    def<int[][]> rows = [[1, 2]]
    rows = [[3, 4], [5, 6]]
    def copy = rows
}
fn<int[2][3]> matrix() = [[1, 2, 3], [4, 5, 6]])");
	bool                 b_found = false;
	for (const decltype(program.arrayConstraints)::value_type &entry : program.arrayConstraints)
		if (entry.first->get_lexeme() == "rows") {
			b_found = true;
			EXPECT_EQ(entry.second.spelling(), "int[][2]");
			EXPECT_EQ(program.localTypes.at(entry.first).spelling(), "int[][]");
		}
	ASSERT_TRUE(b_found);
	EXPECT_EQ(returned(program, "matrix").types.front().element().spelling(), "int[3]");
	expectError("fn run() { def<int[][]> a = [[1,2]]; a = [[3]] }", Code::INVALID_ARRAY);
	expectError("fn run() { def<int[][]> a = [[1,2]]; a[0] = [3] }", Code::INVALID_ARRAY);
	expectError("fn run() { def<int[][]> a = [[1,2],[3]] }", Code::INVALID_ARRAY);
	expectError("fn run() { def<int[][]> a = [[1,2]]; def<int[][]> b = a; b = [[3]] }", Code::INVALID_ARRAY);
	EXPECT_NO_THROW(checkSource("fn<int[][]> make() = [[1,2]]\nfn run() { def a = make(); a = [[3]] }"));
}

TEST (SemanticValues, ArrayMutationsUseTheRetainedElementShape) {
	const CheckedProgram cp_program   = checkSource(R"(fn<int[]> pop(int[][] rows) {
    rows.append([1,2]); rows.insert(0,[3,4]); rows.reserve(8)
    def row = rows.removeAt(1); rows.clear()
    return row
})");
	std::size_t          st_mutations = 0;
	for (const decltype(cp_program.expressions)::value_type &entry : cp_program.expressions)
		if (entry.second.arrayMutation) ++st_mutations;
	EXPECT_EQ(st_mutations, 5u);
	expectError("fn f() { def<int[][]> rows = [[1,2]]; rows.clear(); rows.append([3]) }", Code::INVALID_ARRAY);
	expectError("fn f() { def fixed = [1]; fixed.append(2) }", Code::INVALID_ARRAY_MUTATION);
	expectError("fn f(int[][] rows) { rows[0].clear() }", Code::INVALID_ARRAY_MUTATION);
}

TEST (SemanticValues, ProviderAndRangeSelectionsSurviveChecking) {
	const CheckedProgram program = checkSource(R"(fn<usize> value() = int[2][3]?::size
fn<float<16>> high() = float<16>?::max
fn<range<float<16>>> limits(float<16> high) = 1.0 ..= high
fn<bool> member(int<8> n) = (1 .. 3) == n
fn<typeinfo> element() = int[2]::type::element
fn run() { for (def<float> n <- 0.0 .. 1.0) {} }
)");
	EXPECT_EQ(returned(program).provider->kind, ProviderKind::ArraySize);
	EXPECT_EQ(std::get<IntegerConstant>(returned(program).numericConstant), (IntegerConstant{ { 2 } }));
	EXPECT_EQ(std::get<double>(returned(program, "high").numericConstant), 65504.);
	EXPECT_EQ(returned(program, "limits").range, RangeOperation::Construct);
	EXPECT_EQ(returned(program, "member").range, RangeOperation::Membership);
	EXPECT_EQ(returned(program, "element").representedType, ResolvedType(kInteger));
	ASSERT_EQ(program.iterations.size(), 1u);
	EXPECT_EQ(program.iterations.begin()->second.kind, IterationKind::Range);
	expectError("fn<usize> bad() = string::size", Code::INVALID_PROVIDER_RECEIVER);
	expectError("fn<usize> bad() = int[]::size", Code::INVALID_PROVIDER_RECEIVER);
	expectError("fn<int> bad(int? n) = n::max", Code::NULLABLE_PAYLOAD_ACCESS);
	expectError("fn<typeinfo> bad() = int::type::element", Code::INVALID_METADATA_ACCESS);
	expectError("fn<range<float>> bad() = 1.0 .. (1.0 / 0.0)", Code::INVALID_RANGE_VALUE);
	expectError("fn bad() { for (def n <- (0.0/0.0) .. 1.0) {} }", Code::INVALID_FOR_LOOP);
	expectError("fn bad(int[] values) { for (def<float> n <- values) {} }", Code::INVALID_FOR_LOOP);
	expectError("fn bad(int[] values) { for (def<void> n <- values) {} }", Code::INVALID_FOR_LOOP);
}

TEST (SemanticValues, MetadataFlowsThroughBindingsAndContinuingBranches) {
	expectError("fn<typeinfo> bad() { def d = int::type; return d::element }", Code::INVALID_METADATA_ACCESS);
	expectError("fn<typeinfo> bad(bool flag) { def d = int[1]::type; d = if (flag) { int::type } else { int::type }; return d::element }",
	            Code::INVALID_METADATA_ACCESS);
	const CheckedProgram program   = checkSource("fn<typeinfo> value(bool flag) { def d = int::type; if (flag) { d = int[1]::type }; return d::element }");
	bool                 b_runtime = false;
	for (const decltype(program.expressions)::value_type &entry : program.expressions) {
		const ExpressionInfo &ei_value = entry.second;
		if (ei_value.provider && ei_value.provider->metadata == MetadataQuery::Element) {
			b_runtime = true;
			EXPECT_FALSE(ei_value.representedType);
		}
	}
	EXPECT_TRUE(b_runtime);
	expectError("fn<string> bad(int? n) = n::str", Code::NULLABLE_PAYLOAD_ACCESS);
}

TEST (SemanticValues, InterpolationRetainsTheSelectedFormatter) {
	const CheckedProgram program = checkSource(R"(typedef<class> Text<T> { operator ::str() -> string = "text" }
fn<string> value(Text<int>? item) = #"{item}")");
	ASSERT_EQ(program.textConversions.size(), 1u);
	const ProviderSelection &ps_format = program.textConversions.begin()->second;
	ASSERT_TRUE(ps_format.formatter);
	EXPECT_TRUE(ps_format.receiver.nullable());
	EXPECT_NE(ps_format.formatterInstance, nullptr);
	expectError(R"(typedef<struct> S {}
fn<string> value(S s) = #"{s}")",
	            Code::UNSUPPORTED_INTERPOLATION_TYPE);
}

TEST (SemanticValues, IterationRetainsTheLibrarySnapshotAndSlotDeclarations) {
	const std::filesystem::path p_root = std::filesystem::path(__FILE__).parent_path().parent_path() / "Inputs/collection";
	Lexer                       l_lexer{ SourceFile(p_root / "main.co") };
	const CheckedProgram        cp_program(SymbolTable(Parser(l_lexer).parse()), {}, p_root / "standard");
	ASSERT_EQ(cp_program.iterations.size(), 1u);
	const Iteration &it_plan = cp_program.iterations.begin()->second;
	EXPECT_EQ(it_plan.kind, IterationKind::Snapshot);
	EXPECT_EQ(it_plan.element, ResolvedType(kInteger));
	ASSERT_TRUE(it_plan.snapshot);
	ASSERT_NE(it_plan.snapshot->instance, nullptr);
	EXPECT_EQ(it_plan.snapshot->symbol->name.get_operator(), oForeach);
	ASSERT_NE(it_plan.slots, nullptr);
	EXPECT_EQ(it_plan.slots->visibility, Visibility::Private);
	EXPECT_EQ(it_plan.slots->name.get_lexeme(), "slots");
	ASSERT_NE(it_plan.slotValue, nullptr);
	EXPECT_EQ(it_plan.slotValue->name.get_lexeme(), "value");
}

TEST (SemanticValues, PrivateValidatorsFollowTheSelectedDeclaringSetter) {
	const std::string    base      = R"(typedef<class> Base<T>(int seed) {
    pub Base() : (0) {}
    operator []?(usize i) = true
    pub operator []=(usize i, T value) {}
}
typedef<class> Child : Base<int> { operator []?(usize i) = false }
)";
	const CheckedProgram program   = checkSource(base + "fn write(Child child) { child[0] = 17 }");
	const Symbol        &validator = program.units.front()->get_globals().at("Base").front().members.at(std::string(to_string(oIndexValidate))).front();
	ASSERT_EQ(program.writes.size(), 1u);
	const ExpressionInfo &write = program.writes.begin()->second;
	ASSERT_TRUE(write.indexed);
	EXPECT_EQ(write.indexed->operation, write.symbol);
	EXPECT_EQ(write.indexed->validator, &validator);
	EXPECT_NE(write.indexed->validatorInstance, nullptr);
	expectError(base + "fn bad(Child child) { child[0]++ }", Code::INVALID_INDEX);
	expectError("typedef<class> C { operator []?(int i) = true\npub operator []=(usize i, int v) {} }", Code::INVALID_OPERATOR_DECLARATION);
	expectError(R"(typedef<class> Parent(int seed) {
    pub Parent() : (0) {}
    operator []?(usize i) = true
    pub operator []=(usize i, int value) {}
}
typedef<class> C : Parent {
    operator []?(usize i) = false
    pub operator [](usize i) = 3
}
fn bad(C c) { c[0] += 1 })",
	            Code::INVALID_INDEX);
	expectError("typedef<class> C { def<int> n = 0\noperator []?(int i) -> bool { n++; return true } }", Code::INVALID_OPERATOR_DECLARATION);
	expectError("typedef<struct> Snapshot<T> { T[] slots }\ntypedef<class> Fake { pub operator foreach() -> Snapshot<int> = { [] } }",
	            Code::INVALID_OPERATOR_DECLARATION);
}
}
}
