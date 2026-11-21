// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cmath>

#include <ast/decl/enum_declaration.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/statements.hpp>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {
using Code = cde::DiagnosticCode;

const ExpressionInfo &returned (const CheckedProgram &program, const std::string &name = "value") {
	return program.expressions.at(&bodyExpression(program.units.front()->get_globals().at(name).front()));
}

TEST (SemanticValues, NormalizesIntegerBooleanValuesOnBothTargets) {
	for (unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const CheckedProgram cp_program = checkSource(R"(fn<bool> zero() = 0 == false
fn<bool> falseOne() = false == 1
fn<bool> nonzero() = true == 2
fn<bool> negative() = -2 == true
fn<bool> different() = false != isize::min
fn<bool> same() = usize::max != true
fn<bool> highWord() = int<96>::min -> bool
fn<bool> castZero() = (2 - 2) -> bool
fn<bool> castAndCompare() = (-5 -> bool) == true
fn<bool> wrapped() = ((255 -> unsigned int<8>) + 1) == false
fn<bool> dynamic(bool flag, int n) = n == flag
fn<bool> lifted(bool? flag, int? n) = flag != n
fn<bool?> nullable(int? n) = n
fn<bool> defaulted(opt<bool> value ?: -3) = value
fn<bool> inputs(int n, unsigned int<12> u, isize s, usize z) {
    bool x = 0
    x = n
    bool[3] a = [u, s, z]
    a[1] = n
    return x
}
fn<int> branch() { if (true == -9) return 7 }
fn<int> skipped() { if (false == 1) {} else return 7 }
fn<int> loop() { do { return 7 } while (false == 1) }
)",
		                                              { u_bits });
		for (const char *s_name : { "zero", "nonzero", "negative", "different", "highWord", "castAndCompare", "wrapped" }) {
			SCOPED_TRACE(s_name);
			EXPECT_EQ(returned(cp_program, s_name).numericConstant, NumericConstant(IntegerConstant{ { 1 } }));
		}
		for (const char *s_name : { "falseOne", "same", "castZero" }) {
			SCOPED_TRACE(s_name);
			EXPECT_EQ(returned(cp_program, s_name).numericConstant, NumericConstant(IntegerConstant{ { 0 } }));
		}
		for (const char *s_name : { "dynamic", "lifted" }) {
			EXPECT_TRUE(returned(cp_program, s_name).booleanEquality);
			EXPECT_FALSE(returned(cp_program, s_name).integerTruth().has_value());
		}
		const SymbolScope  &ss_globals  = cp_program.units.front()->get_globals();
		const ResolvedType &rt_nullable = cp_program.declarations.at(&ss_globals.at("nullable").front()).returns.front();
		EXPECT_TRUE(rt_nullable.convertsIntegerToBool(returned(cp_program, "nullable").types.front()));
		const CallableSignature  &cs_inputs = *std::get<const CallableSignature *>(ss_globals.at("inputs").front().declaration);
		const BindingDeclaration &bd_binding =
		        dynamic_cast<const BindingDeclaration &>(*std::get<BlockBody>(cs_inputs.executableBody()->value).statements.front());
		const ExpressionInfo &ei_initializer = cp_program.expressions.at(bd_binding.initializer.get());
		EXPECT_EQ(cp_program.localTypes.at(&bd_binding.name), ResolvedType(kBoolean));
		EXPECT_EQ(ei_initializer.types.front(), ResolvedType(kInteger));
		EXPECT_EQ(ei_initializer.integerTruth(), false);
	}
	expectError("fn<bool> f(float x) = x", Code::RETURN_TYPE);
	expectError("fn<bool> f(int? x) = x", Code::RETURN_TYPE);
	expectError("fn<bool> f(float x) = false == x", Code::INCOMPARABLE_TYPES);
	expectError("fn<bool> f(int x) = false === x", Code::INCOMPARABLE_TYPES);
	expectError("fn<bool> f(int[] x, bool[] flags) = x == flags", Code::INCOMPARABLE_TYPES);
	expectError("fn<bool> f(int n) = !n", Code::UNARY_OPERAND);
	expectError("fn<int> f() { if (false == 1) return 1 }", Code::MISSING_RETURN);
}

TEST (SemanticValues, PrefersIntegerOverloadsBeforeBooleanConversion) {
	const CheckedProgram cp_program = checkSource(R"(fn<int> pick(int n) = n
fn<bool> pick(bool b) = b
fn<int<8>> narrow(int<8> n) = n
fn<bool> narrow(bool b) = b
fn<bool> accept(bool b) = b
fn<bool> choose(bool b) = b
fn<T> choose<T>(T n) = n
fn<int> integer() = pick(0)
fn<bool> boolean() = pick(false)
fn<int<8>> contextual() = narrow(1)
fn<int> inferred() = choose(1)
fn<bool> converted(int n) = accept(n)
fn<int> arrayPick(int<8>[] values) = 1
fn<bool> arrayPick(bool[] flags) = true
fn<int> arrayLiteral() = arrayPick([1, 2])
typedef<struct> Flag {
    pub operator ==(bool b) = b
    pub operator ==(int<8> n) = false
}
fn<bool> operatorLiteral(Flag f) = f == 2
fn<bool> operatorFlag(Flag f) = f == true
fn<bool> operatorConverted(Flag f, usize n) = f == n
)");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	EXPECT_EQ(returned(cp_program, "integer").argumentContract, &ss_globals.at("pick")[0]);
	EXPECT_EQ(returned(cp_program, "boolean").argumentContract, &ss_globals.at("pick")[1]);
	EXPECT_EQ(returned(cp_program, "contextual").argumentContract, &ss_globals.at("narrow")[0]);
	EXPECT_EQ(returned(cp_program, "inferred").argumentContract, &ss_globals.at("choose")[1]);
	EXPECT_EQ(returned(cp_program, "converted").argumentContract, &ss_globals.at("accept")[0]);
	EXPECT_EQ(returned(cp_program, "arrayLiteral").argumentContract, &ss_globals.at("arrayPick")[0]);
	const ExpressionInfo &ei_flag = returned(cp_program, "operatorFlag");
	ASSERT_NE(ei_flag.argumentContract, nullptr);
	EXPECT_NE(returned(cp_program, "operatorLiteral").argumentContract, ei_flag.argumentContract);
	EXPECT_EQ(returned(cp_program, "operatorConverted").argumentContract, ei_flag.argumentContract);
	EXPECT_FALSE(returned(cp_program, "operatorConverted").booleanEquality);
	expectError("typedef<struct> F { pub operator ==(bool b) = b\n operator ==(int n) = true }\nfn<bool> check(F f) = f == 1", Code::PRIVATE_CLASS_MEMBER);
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

TEST (SemanticValues, ComplementsIntegerConstantsAtTheOperandWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const CheckedProgram cp_program = checkSource(R"(fn<int> value() = ~-43
fn<int> product() = ~2 * 3
fn<int<8>> minimum() = ~127
fn<int<8>> maximum() = ~-128
fn<unsigned int<12>> narrow() = ~0
fn<unsigned int<65>> wide() = ~1
fn<usize> sized() = ~0
fn<int> computedDivisor() = 1 / ~-1
)",
		                                              { u_bits });
		EXPECT_EQ(returned(cp_program).numericConstant, NumericConstant(IntegerConstant{ { 42 } }));
		EXPECT_EQ(returned(cp_program, "product").numericConstant, NumericConstant(IntegerConstant{ { 9 }, true }));
		EXPECT_EQ(returned(cp_program, "minimum").numericConstant, NumericConstant(IntegerConstant{ { 128 }, true }));
		EXPECT_EQ(returned(cp_program, "maximum").numericConstant, NumericConstant(IntegerConstant{ { 127 } }));
		EXPECT_EQ(returned(cp_program, "narrow").numericConstant, NumericConstant(IntegerConstant{ { 4095 } }));
		EXPECT_EQ(returned(cp_program, "wide").numericConstant, NumericConstant(IntegerConstant{ { UINT32_MAX - 1, UINT32_MAX, 1 } }));
		EXPECT_EQ(returned(cp_program, "sized").numericConstant, NumericConstant(IntegerConstant{ IntegerValue(u_bits / 32, UINT32_MAX) }));
		EXPECT_TRUE(std::holds_alternative<std::monostate>(returned(cp_program, "computedDivisor").numericConstant));
	}
	expectError("fn<float> f(float x) = ~x", Code::UNARY_OPERAND);
	expectError("fn<bool> f(bool x) = ~x", Code::UNARY_OPERAND);
	expectError("fn<int> f() = ~2147483648", Code::INTEGER_RANGE);
	expectError("typedef<class> C { operator ~(int x) = x }", Code::INVALID_OPERATOR_DECLARATION);
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

TEST (SemanticValues, OverloadTrialsCheckConstantArithmeticAtEachParameterWidth) {
	for (unsigned u_bits : { 32u, 64u }) {
		const CheckedProgram cp_program = checkSource(R"(fn<bool> pick(int<8> n) = false
fn<string> pick(opt<bool> flag ?: false, int<16> n) = "wide"
fn<string> quotient() = pick(n = -128 / -1)
fn<string> remainder() = pick(n = -128 % -1)
typedef<class> Offset {
    pub operator +(int<8> n) -> bool = false
    pub operator +(int<16> n) -> string = "wide"
}
fn<string> offset(Offset value) = value + (-128 / -1)
fn<bool> limits(range<float<16>> r) = false
fn<string> limits(range<float<64>> r) = "wide"
fn<string> finite() = limits(0.0 .. (50000.0 * 2.0)))",
		                                              { u_bits });
		const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
		for (const std::string s_name : { "quotient", "remainder" }) {
			const CallExpression &ce_call = dynamic_cast<const CallExpression &>(bodyExpression(ss_globals.at(s_name).front()));
			const ExpressionInfo &ei_call = cp_program.expressions.at(&ce_call);
			EXPECT_EQ(ei_call.argumentContract, &ss_globals.at("pick")[1]);
			EXPECT_EQ(ei_call.parameterForArgument, (std::vector<std::size_t>{ 1 }));
			EXPECT_EQ(ei_call.omittedParameters, (std::vector<std::size_t>{ 0 }));
			const ExpressionInfo &ei_argument = cp_program.expressions.at(ce_call.suppliedArguments.front().expression.get());
			EXPECT_EQ(ei_argument.types.front().width, 16u);
			EXPECT_EQ(std::get<IntegerConstant>(ei_argument.numericConstant).magnitude, IntegerValue{ s_name == "quotient" ? 128u : 0u });
		}
		EXPECT_EQ(returned(cp_program, "offset").argumentContract, &ss_globals.at("Offset").front().members.at(std::string(to_string(oPlus)))[1]);
		EXPECT_EQ(returned(cp_program, "finite").argumentContract, &ss_globals.at("limits")[1]);
	}
	expectError("fn take(int<8> n) {}\nfn f() { take(-128 % -1) }", Code::NO_MATCHING_CALL);
	expectError("fn take(int<8> n) {}\nfn take(int<16> n) {}\nfn f() { take(2 / 0) }", Code::INTEGER_ZERO_DIVISOR);
	expectError("fn take(int<8> n) {}\nfn take(int<16> n) {}\nfn f() { take(12) }", Code::AMBIGUOUS_CALL);
}

TEST (SemanticValues, OrdinaryMatchesPrecedeContextualTrialsAndTrialsRetypeEveryArgument) {
	const CheckedProgram cp_program = checkSource(R"(fn<bool> probe(float x, int<8> n) = false
fn<string> probe(float<16> x, int<16> n) = "half"
fn<string> replay(float<16> h) = probe(h + 0.5, 2)
fn<string> accept(float x, int? n) = "natural"
fn<bool> accept(float<16> x, int<16> n) = false
fn<string> ordinary(float<16> h) = accept(h + 0.5, 2))");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	EXPECT_EQ(returned(cp_program, "replay").argumentContract, &ss_globals.at("probe")[1]);
	EXPECT_EQ(returned(cp_program, "ordinary").argumentContract, &ss_globals.at("accept").front());
	const std::string s_group = "fn take(int<8> n) {}\nfn take(int<16> n) {}\n";
	expectError(s_group + "fn f() { take(if(1) { 2 } else { 3 }) }", Code::NO_MATCHING_CALL);
	expectError(s_group + "fn f() { take((if(1) { 2 } else { 3 }).abs()) }", Code::NON_BOOLEAN_CONDITION);
}

TEST (SemanticValues, FloatingLiteralContextIsIndependentOfIntegerPeers) {
	const CheckedProgram    cp_program = checkSource(R"(fn<bool> take(float<64> x) = true
fn<bool> value(int<8> i) = take((1 + i) + 16777217.0)
fn<float<64>> reversed(int<8> i) = 16777217.0 + (1 + i)
fn<float> natural(float<16> h) { def v = h + 1.0; return v }
fn<float<16>> narrow(float<16> h) = h + 1.0
fn<bool> compare(int<64> n) = (2147483648 + 1) < n)");
	const SymbolScope      &ss_globals = cp_program.units.front()->get_globals();
	const CallExpression   &ce_call    = dynamic_cast<const CallExpression &>(bodyExpression(ss_globals.at("value").front()));
	const BinaryExpression &be_sum     = dynamic_cast<const BinaryExpression &>(*ce_call.suppliedArguments.front().expression);
	EXPECT_EQ(cp_program.expressions.at(be_sum.left.get()).types.front().width, 8u);
	EXPECT_EQ(cp_program.expressions.at(&be_sum).types.front().width, 64u);
	EXPECT_EQ(std::get<double>(cp_program.expressions.at(be_sum.right.get()).numericConstant), 16777217.);
	EXPECT_EQ(returned(cp_program, "narrow").types.front().width, 16u);
	expectError("fn<float<64>> f(float x) = x + 1.0", Code::RETURN_TYPE);
	expectError("fn<float<64>> f(int<8> x) = (128 + x) + 1.0", Code::INTEGER_RANGE);
}

TEST (SemanticValues, NestedArithmeticUsesTheSelectedOperatorResultAsItsPeer) {
	std::string s_source = "typedef<class> Offset { pub operator +(int<64> n) -> int<64> = n }\nfn<int<64>> value(Offset base) = base";
	for (unsigned u_term = 0; u_term < 100; ++u_term) s_source += " + 1";
	s_source += " + 2147483648";
	const CheckedProgram cp_program = checkSource(s_source);
	EXPECT_EQ(returned(cp_program).types.front().width, 64u);
}

TEST (SemanticValues, ArrayInferenceUsesTypedBranchTailsBeforeDefaultingLiterals) {
	const CheckedProgram cp_program = checkSource(R"(fn<bool> use(int[] row) = false
fn<string> use(int<64>[] row) = "wide"
fn<string> wide(bool flag) = use([2147483648, if(flag) { int<64> n = 2; n } else { 1 }])
fn<bool> ordinary() = use([1, 2])
fn matrix(int<64> n) { def rows = [[1, 2], [3, n]] }
fn ranges(range<int<64>> r) { def intervals = [1 .. 2, r] }
)");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	EXPECT_EQ(returned(cp_program, "wide").argumentContract, &ss_globals.at("use")[1]);
	EXPECT_EQ(returned(cp_program, "ordinary").argumentContract, &ss_globals.at("use").front());
	const decltype(cp_program.localTypes)::const_iterator it_rows = std::ranges::find_if(
	        cp_program.localTypes, [] (const decltype(cp_program.localTypes)::value_type &entry) { return entry.first->get_lexeme() == "rows"; });
	ASSERT_NE(it_rows, cp_program.localTypes.end());
	EXPECT_EQ(it_rows->second.spelling(), "int<64>[2][2]");
	expectError("fn f(int<64> n) { def rows = [[1], [2,n]] }", Code::INVALID_ARRAY);
	expectError("fn f(float<64> wide) { def vals = [1.0,wide] }", Code::INVALID_ARRAY);
}

TEST (SemanticValues, ArrayEqualityChecksFloatingLiteralsAtThePeerElementWidth) {
	const CheckedProgram cp_program = checkSource(R"(fn<bool> left(float<16>[]? row) = [1.000488281250000001, -0.0] == row
fn<bool> right(float<16>[]? row) = row != [1.000488281250000001, -0.0]
fn<bool> nested(float<64> wide) = [[16777217.0]] == [[wide]]
fn<float<16>[]> choose(int<8> n) = [0.0]
fn<float<64>[]> choose(int<16> n) = [0.0]
fn<bool> selected() = [16777217.0] == choose(300))");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	for (const std::string s_name : { "left", "right" }) {
		const BinaryExpression &be_compare = dynamic_cast<const BinaryExpression &>(bodyExpression(ss_globals.at(s_name).front()));
		const ArrayLiteral     &al_row     = dynamic_cast<const ArrayLiteral &>(s_name == "left" ? *be_compare.left : *be_compare.right);
		const ExpressionInfo   &ei_first   = cp_program.expressions.at(al_row.elements[0].get()),
		                     &ei_zero      = cp_program.expressions.at(al_row.elements[1].get());
		EXPECT_EQ(ei_first.types.front().width, 16u);
		EXPECT_EQ(std::get<double>(ei_first.numericConstant), 1.0009765625);
		EXPECT_EQ(ei_zero.types.front().width, 16u);
		EXPECT_TRUE(std::signbit(std::get<double>(ei_zero.numericConstant)));
	}
	const BinaryExpression &be_nested = dynamic_cast<const BinaryExpression &>(bodyExpression(ss_globals.at("nested").front()));
	const ArrayLiteral     &al_outer  = dynamic_cast<const ArrayLiteral &>(*be_nested.left);
	const ArrayLiteral     &al_inner  = dynamic_cast<const ArrayLiteral &>(*al_outer.elements.front());
	const ExpressionInfo   &ei_wide   = cp_program.expressions.at(al_inner.elements.front().get());
	EXPECT_EQ(ei_wide.types.front().width, 64u);
	EXPECT_EQ(std::get<double>(ei_wide.numericConstant), 16777217.);
	const BinaryExpression &be_selected = dynamic_cast<const BinaryExpression &>(bodyExpression(ss_globals.at("selected").front()));
	const ArrayLiteral     &al_selected = dynamic_cast<const ArrayLiteral &>(*be_selected.left);
	EXPECT_EQ(cp_program.expressions.at(be_selected.right.get()).argumentContract, &ss_globals.at("choose")[1]);
	EXPECT_EQ(std::get<double>(cp_program.expressions.at(al_selected.elements.front().get()).numericConstant), 16777217.);
	expectError("fn<bool> f(float<16>[] row) = row == [70000.0]", Code::FLOAT_RANGE);
	expectError("fn<bool> f(float<16>[] row, float<64> wide) = [wide] == row", Code::INVALID_ARRAY);
}

TEST (SemanticValues, RangeComparisonsRecordTheSelectedEndpointType) {
	const CheckedProgram cp_program = checkSource(R"(fn<bool> stored(range<float<64>> r) = 16777217.0 == r
fn<bool> inlineRange(float<16> high) = (1.000488281250000001 ..= high) != -0.0
fn<bool> conditional(bool flag, range<int<8>> r) = (if(flag) 0..3 else 1..4) == r
fn<bool> exiting() = (if(true) { return true } else { false }) == 1..2
typedef<struct> Cursor { pub operator ..(int last) -> range<int<8>> = 0..1 }
fn<bool> overloaded(Cursor cur) = (cur .. 2) == 1)");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	const std::function  comparison = [&] (const std::string &name) -> const BinaryExpression  &{
		return dynamic_cast<const BinaryExpression &>(bodyExpression(ss_globals.at(name).front()));
	};
	const BinaryExpression &be_stored = comparison("stored"), &be_inline = comparison("inlineRange"), &be_conditional = comparison("conditional");
	const ExpressionInfo   &ei_wide = cp_program.expressions.at(be_stored.left.get()), &ei_zero = cp_program.expressions.at(be_inline.right.get());
	EXPECT_EQ(cp_program.expressions.at(&be_stored).range, RangeOperation::Membership);
	EXPECT_EQ(ei_wide.types.front().width, 64u);
	EXPECT_EQ(std::get<double>(ei_wide.numericConstant), 16777217.);
	EXPECT_EQ(cp_program.expressions.at(&be_inline).range, RangeOperation::Membership);
	EXPECT_EQ(ei_zero.types.front().width, 16u);
	EXPECT_TRUE(std::signbit(std::get<double>(ei_zero.numericConstant)));
	const BinaryExpression &be_bounds = dynamic_cast<const BinaryExpression &>(*be_inline.left);
	const ExpressionInfo   &ei_low    = cp_program.expressions.at(be_bounds.left.get());
	EXPECT_EQ(cp_program.expressions.at(&be_bounds).range, RangeOperation::Construct);
	EXPECT_EQ(ei_low.types.front().width, 16u);
	EXPECT_EQ(std::get<double>(ei_low.numericConstant), 1.0009765625);
	EXPECT_EQ(cp_program.expressions.at(&be_conditional).range, RangeOperation::Equal);
	EXPECT_EQ(cp_program.expressions.at(be_conditional.left.get()).types.front().arguments().front().width, 8u);
	EXPECT_FALSE(returned(cp_program, "exiting").fallsThrough);
	const BinaryExpression &be_overloaded = comparison("overloaded");
	EXPECT_EQ(cp_program.expressions.at(be_overloaded.left.get()).argumentContract,
	          &ss_globals.at("Cursor").front().members.at(std::string(to_string(oRange))).front());
	EXPECT_EQ(cp_program.expressions.at(be_overloaded.right.get()).types.front().width, 8u);
}

TEST (SemanticValues, RangeMembershipValidatesExactTypesAndTargetLimits) {
	const std::string s_endpoint = "fn<bool> f(float<16> x, float<64> high) = x == 0.0..high";
	try {
		checkSource(s_endpoint);
		FAIL() << "Expected an endpoint type mismatch";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), Code::INVALID_RANGE_MEMBERSHIP);
		ASSERT_FALSE(error.get_source_labels().empty());
		const TokenSpan &ts_bound = error.get_source_labels().front().span;
		EXPECT_EQ(ts_bound.startCol, s_endpoint.rfind("high") + 1);
		EXPECT_EQ(ts_bound.endCol, s_endpoint.size() + 1);
	}
	expectError("fn<bool> f(float<16> x, float<64> high) = (x..1.0) == (0.0..high)", Code::INVALID_RANGE_VALUE);
	expectError("fn<bool> f(float<16> x) = x == (0.0 .. (50000.0 * 2.0))", Code::INVALID_RANGE_MEMBERSHIP);
	expectError("fn<bool> f(int<8> low) = (low..true) == 128", Code::INTEGER_RANGE);
	expectError("fn<bool> f(float<16> x) = (if(true) 1.0 else x) == 0.0..1.5", Code::BRANCH_TYPE_MISMATCH);
	expectError("fn accept(bool ok) {}\nfn f(range<int<8>> r) { accept(r != 128) }", Code::INTEGER_RANGE);
	expectError("fn<bool> f(range<int<8>>? r) = 1 == r", Code::NULLABLE_PAYLOAD_ACCESS);
	const std::string s_source = "fn<bool> f(range<usize> r) = 4294967296 == r";
	EXPECT_NO_THROW(checkSource(s_source, { 64 }));
	try {
		checkSource(s_source, { 32 });
		FAIL() << "Expected the 32-bit endpoint limit";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), Code::INTEGER_RANGE);
	}
}

TEST (SemanticValues, NullableFallbacksCarryTheDestinationIntoEachBranch) {
	const CheckedProgram cp_program = checkSource(R"(fn<int?> value(bool flag, int? x) = x ?: if(flag) { null } else { 2 }
fn<int?[]> row(bool flag) = [if(flag) { null } else { 2 }]
fn<int?> left(bool flag, int? x) = (if(flag) { x } else { null }) ?: 5
fn broadcast(bool flag, int? x) { int? a, b = 1; a, b = x ?: if(flag) { null } else { 2 } }
)");
	EXPECT_TRUE(returned(cp_program).types.front().nullable());
	EXPECT_TRUE(returned(cp_program, "row").types.front().element().nullable());
	EXPECT_EQ(returned(cp_program, "left").types.front(), ResolvedType(kInteger));
	expectError("fn f(bool flag) { def x = [if(flag) { null } else { 3 }] }", Code::EXPLICIT_NULLABLE_TYPE);
	expectError("fn f(int? x) { println(x ?: true) }", Code::INVALID_ELVIS);
	expectError("fn f() { print(null ?: null) }", Code::EXPLICIT_NULLABLE_TYPE);
}

TEST (SemanticValues, RangeCompoundStepsRetainTheirTypedUpdate) {
	const CheckedProgram cp_program = checkSource("fn f() { for (float i <- 0.0 .. 4.0; i += 1) {} }");
	ASSERT_EQ(cp_program.iterations.size(), 1u);
	const ForStepper &fs_step = *cp_program.iterations.begin()->first->stepper;
	ASSERT_TRUE(fs_step.compound);
	EXPECT_EQ(fs_step.compound->op.get_operator(), oPlus);
	EXPECT_EQ(cp_program.expressions.at(fs_step.update()).types.front(), ResolvedType(kFloat));
	EXPECT_EQ(cp_program.expressions.at(fs_step.value.get()).types.front(), ResolvedType(kInteger));
	expectError("fn f() { for (float i <- 0.0 .. 4.0; i = 1) {} }", Code::TYPE_MISMATCH);
	expectError("fn f() { for (float<16> i <- 0.0 .. 4.0; i += 1) {} }", Code::TYPE_MISMATCH);
	expectError("fn f() { for (int i <- 0 .. 4; i /= 0) {} }", Code::INTEGER_ZERO_DIVISOR);
}

TEST (SemanticValues, ArrayBindingsKeepInnerExtentsAndSeparateCountsFromDeclaredTypes) {
	const CheckedProgram program = checkSource(R"(fn run() {
    int[][] rows = [[1, 2]]
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
	expectError("fn run() { int[][] a = [[1,2]]; a = [[3]] }", Code::INVALID_ARRAY);
	expectError("fn run() { int[][] a = [[1,2]]; a[0] = [3] }", Code::INVALID_ARRAY);
	expectError("fn run() { int[][] a = [[1,2],[3]] }", Code::INVALID_ARRAY);
	expectError("fn run() { int[][] a = [[1,2]]; int[][] b = a; b = [[3]] }", Code::INVALID_ARRAY);
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
	expectError("fn f() { int[][] rows = [[1,2]]; rows.clear(); rows.append([3]) }", Code::INVALID_ARRAY);
	expectError("fn f() { def fixed = [1]; fixed.append(2) }", Code::INVALID_ARRAY_MUTATION);
	expectError("fn f(int[][] rows) { rows[0].clear() }", Code::INVALID_ARRAY_MUTATION);
}

TEST (SemanticValues, ProviderAndRangeSelectionsSurviveChecking) {
	const CheckedProgram program = checkSource(R"(fn<usize> value() = int[2][3]?::size
fn<float<16>> high() = float<16>?::max
fn<range<float<16>>> limits(float<16> high) = 1.0 ..= high
fn<bool> member(int<8> n) = (1 .. 3) == n
fn<typeinfo> element() = int[2]::type::element
fn run() { for (float n <- 0.0 .. 1.0) {} }
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
	expectError("fn bad(int[] values) { for (float n <- values) {} }", Code::INVALID_FOR_LOOP);
	expectError("fn bad(int[] values) { for (void n <- values) {} }", Code::INVALID_FOR_LOOP);
}

TEST (SemanticValues, ProviderDefaultsUseOnlyValuesAlreadyInScope) {
	const CheckedProgram cp_program = checkSource(R"(typedef<struct> Point { int x }
fn<typeinfo> current(opt<typeinfo> Point ?: Point::type) = Point
fn<typeinfo> later(opt<typeinfo> d ?: Point::type, int Point) = d
fn<typeinfo> earlier(int Point, opt<typeinfo> d ?: Point::type) = d
)");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	for (const std::string s_name : { "current", "later", "earlier" }) {
		SCOPED_TRACE(s_name);
		const CallableSignature &cs_function = *std::get<const CallableSignature *>(ss_globals.at(s_name).front().declaration);
		const ExpressionInfo    &ei_info     = cp_program.expressions.at(cs_function.parameters.at(s_name == "earlier" ? 1 : 0).defaultValue.get());
		ASSERT_TRUE(ei_info.provider);
		EXPECT_EQ(ei_info.provider->typeReceiver, s_name != "earlier");
		ASSERT_TRUE(ei_info.representedType);
		if (s_name == "earlier")
			EXPECT_EQ(ei_info.representedType, ResolvedType(kInteger));
		else
			EXPECT_EQ(ei_info.representedType->declaration, &ss_globals.at("Point").front());
	}
	expectError("typedef<class> Point {}\nfn f(opt<int> Point ?: Point) {}", Code::INVALID_PARAMETER_DEFAULT);
	expectError("typedef<class> Point {}\nfn f(opt<int> d ?: Point, int Point) {}", Code::INVALID_PARAMETER_DEFAULT);
}

TEST (SemanticValues, QualifiedMetadataUsesAnAvailableValueBeforeANestedType) {
	const CheckedProgram  cp_program = checkSource(R"(typedef<class> Outer { pub typedef<class> Leaf {} }
typedef<struct> Fields { pub int Leaf }
fn<typeinfo> nested() = Outer.Leaf::type
fn<typeinfo> shadow(Fields Outer) = Outer.Leaf::type
)");
	const ExpressionInfo &ei_nested  = returned(cp_program, "nested");
	const ExpressionInfo &ei_shadow  = returned(cp_program, "shadow");
	ASSERT_TRUE(ei_nested.representedType);
	ASSERT_TRUE(ei_nested.provider);
	ASSERT_TRUE(ei_shadow.provider);
	const Symbol &sym_outer = cp_program.units.front()->get_globals().at("Outer").front();
	EXPECT_EQ(ei_nested.representedType->declaration, &sym_outer.members.at("Leaf").front());
	EXPECT_EQ(ei_shadow.representedType, ResolvedType(kInteger));
	EXPECT_TRUE(ei_nested.provider->typeReceiver);
	EXPECT_FALSE(ei_shadow.provider->typeReceiver);
	expectError("typedef<class> Outer { pub typedef<class> Leaf {} }\nfn f(int Outer) { Outer.Leaf::type }", Code::INVALID_STRUCT_FIELD);
	expectError("typedef<class> Outer {}\nfn f(Outer value) { value.missing::type }", Code::INVALID_CLASS_MEMBER);
	expectError("typedef<enum> E { A }\nfn f() { E.missing::type }", Code::INVALID_ENUM_MEMBER);
}

TEST (SemanticValues, TopLevelStateQualifiesTheSameTypeInSignaturesMetadataAndTests) {
	const CheckedProgram   cp_program = checkSource(R"(fn<Ready.Data> echo(Ready.Data data) = data
fn<typeinfo> metadata() = Ready.Data::type
fn<bool> matches(Object value) = value is Ready.Data
fn<typeinfo> shadow(Fields Ready) = Ready.Data::type
fn<bool> compare(Choice choice, Fields Ready) = choice is Ready.Data
pub typedef<class> Host { default state Ready }
typedef<state<void>> pub Ready for Host { pub typedef<struct> Data {} }
typedef<class> Choice { pub operator is(Choice other) = true }
typedef<struct> Fields { pub Choice Data })");
	const SymbolScope     &ss_globals = cp_program.units.front()->get_globals();
	const Symbol          &sym_data   = ss_globals.at("Ready").front().members.at("Data").front();
	const DeclarationInfo &di_echo    = cp_program.declarations.at(&ss_globals.at("echo").front());
	ASSERT_EQ(di_echo.parameters.size(), 1u);
	EXPECT_EQ(di_echo.parameters.front().declaration, &sym_data);
	EXPECT_EQ(di_echo.returns, di_echo.parameters);
	const ExpressionInfo &ei_metadata = returned(cp_program, "metadata"), &ei_shadow = returned(cp_program, "shadow");
	EXPECT_EQ(ei_metadata.representedType, di_echo.parameters.front());
	EXPECT_EQ(returned(cp_program, "matches").testedType, di_echo.parameters.front());
	ASSERT_TRUE(ei_metadata.provider);
	EXPECT_TRUE(ei_metadata.provider->typeReceiver);
	ASSERT_TRUE(ei_shadow.representedType);
	EXPECT_EQ(ei_shadow.representedType->declaration, &ss_globals.at("Choice").front());
	ASSERT_TRUE(ei_shadow.provider);
	EXPECT_FALSE(ei_shadow.provider->typeReceiver);
	EXPECT_EQ(returned(cp_program, "compare").argumentContract, &ss_globals.at("Choice").front().members.at(std::string(to_string(kIs))).front());
}

TEST (SemanticValues, StateTypePathsRespectLexicalVisibilityAndNominalTypePrecedence) {
	const std::string s_state = R"(pub typedef<class> Host { default state Ready }
typedef<state<void>> pub Ready for Host {
    typedef<class> Hidden { pub typedef<struct> Leaf {} }
    pub typedef<struct> Data {}
    mt<Hidden.Leaf> keep(Hidden.Leaf value) = value
}
)";
	expectError(s_state + "fn<typeinfo> f() = Ready.Hidden.Leaf::type", Code::PRIVATE_NESTED_TYPE);
	expectError(s_state + "fn f(Ready stateValue) {}", Code::UNSUPPORTED_TYPE);
	expectError(s_state + "fn f(Host.Ready.Data value) {}", Code::UNSUPPORTED_TYPE);
	const CheckedProgram cp_program = checkSource(s_state + "typedef<class> Ready { pub typedef<class> Data {} }\nfn<typeinfo> f() = Ready.Data::type");
	const std::vector<Symbol>                &v_names = cp_program.units.front()->get_globals().at("Ready");
	const std::vector<Symbol>::const_iterator it_type = std::ranges::find(v_names, SymbolKind::Type, &Symbol::kind);
	ASSERT_NE(it_type, v_names.end());
	const std::optional<ResolvedType> &opt_data = returned(cp_program, "f").representedType;
	ASSERT_TRUE(opt_data);
	EXPECT_EQ(opt_data->declaration, &it_type->members.at("Data").front());
}

TEST (SemanticValues, MetadataValidatesKnownTypesAndPreservesSymbolicArrayElements) {
	for (unsigned u_bits : { 32u, 64u }) {
		const CheckedProgram cp_program = checkSource(R"(fn<typeinfo> value() = void::type
fn<usize> rank() = void::type::rank
fn<string> name() = void::type::str
fn<typeinfo> element<T>(T sample) = T[]::type::element
fn<usize> inspect(typeinfo descriptor) = descriptor::width
)",
		                                              { u_bits });
		EXPECT_EQ(returned(cp_program).representedType, ResolvedType(kVoid));
		EXPECT_EQ(returned(cp_program, "rank").types, ResultTypes{ ResolvedType(kUsize) });
		const Symbol                      &sym_element     = cp_program.units.front()->get_globals().at("element").front();
		const std::optional<ResolvedType> &opt_represented = returned(cp_program, "element").representedType;
		ASSERT_TRUE(opt_represented);
		EXPECT_EQ(*opt_represented, cp_program.declarations.at(&sym_element).parameters.front());
	}
	expectError("fn f() { void?::type }", Code::INVALID_NULLABLE_TYPE);
	expectError("fn f() { null::type }", Code::EXPLICIT_NULLABLE_TYPE);
	expectError("fn f<T>() { T::type::width }", Code::INVALID_METADATA_ACCESS);
	const std::string s_overloads = "fn pick(string s) {}\nfn pick(char ch) {}\n";
	expectError(s_overloads + "fn f() { pick((bool::type -> typeinfo)::width) }", Code::INVALID_METADATA_ACCESS);
	expectError(s_overloads + "fn f() { def d = bool::type; pick(d::width) }", Code::NO_MATCHING_CALL);
	expectError("fn pick(bool b) {}\nfn f() { def d = bool::type; pick(d::width) }", Code::INVALID_METADATA_ACCESS);
}

TEST (SemanticValues, MetadataTracksContinuingPathsAndDeferredReads) {
	expectError("fn f() { (null ?: bool::type)::width }", Code::INVALID_METADATA_ACCESS);
	expectError("fn f() { def d := bool::type; usize read = { return d::width } }", Code::INVALID_METADATA_ACCESS);
	expectError("fn f() { def d = bool::type; do { d::width } while (false) }", Code::INVALID_METADATA_ACCESS);
	EXPECT_NO_THROW(checkSource(R"(typedef<class> Holder { pub def<typeinfo> d = bool::type }
fn guarded(Holder? h) { (h?.d::type ?: typeinfo::type)::width }
fn deferred() { def d = bool::type; usize read = { return d::width }; read; d::width }
fn repeat(bool flag) { def d = bool::type; do { d::width } while (flag) }
fn iterate() { def d = bool::type; for (def i <- [1, 2]) { d::width }; d::width }
)"));
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

TEST (SemanticValues, ReadOnlyDefaultsRejectUserFormattersAtTheReceiver) {
	const std::string s_types = R"(typedef<class> Item { pub operator ::str() = "item" }
typedef<class> Holder(pub def<Item> item) {}
)";
	try {
		checkSource(s_types + "fn format(Holder h,\n    opt<string> label ?: h.item::str) {}");
		FAIL() << "Expected a read-only formatter diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), Code::INVALID_PARAMETER_DEFAULT) << error.what();
		const std::vector<cde::SourceLabel> &v_labels = error.get_source_labels();
		ASSERT_EQ(v_labels.size(), 2u);
		EXPECT_EQ(v_labels[0].span.to_string(), "4:26-4:32");
		EXPECT_EQ(v_labels[1].span.to_string(), "4:17-4:22");
	}
	expectError(s_types + "fn format(Item?[] items, opt<string> s ?: items::str) {}", Code::INVALID_PARAMETER_DEFAULT);
	expectError(s_types + "fn format(Holder? h, opt<string?> s ?: h?.item::str) {}", Code::INVALID_PARAMETER_DEFAULT);
	expectError(s_types + "fn format(Item? item, opt<string> s ?: item::str) {}", Code::NULLABLE_PAYLOAD_ACCESS);
	expectError(R"(typedef<struct> Item { pub operator ::str() = "item" }
fn format(Item item, opt<string> s ?: item::str) {})",
	            Code::INVALID_PARAMETER_DEFAULT);
	EXPECT_NO_THROW(checkSource(s_types + R"(fn format(Item item, opt<string> s ?: item::type::str) {}
fn<Item> copy(Item item) = item
fn temporary(Item item, opt<string> s ?: copy(item)::str) {})"));
}

TEST (SemanticValues, ReadOnlyFormattingUsesKnownClassApplications) {
	const std::string        s_program  = R"(typedef<interface> View<T> {}
typedef<class> Text<T> : View<T> { pub operator ::str() = "text" }
fn format(View<int> item, opt<string> direct ?: item::str, opt<string> interpolated ?: #"{item}") {}
)";
	const CheckedProgram     cp_program = checkSource(s_program + "fn other(Text<bool> text) {}");
	const CallableSignature &cs_format  = *std::get<const CallableSignature *>(cp_program.units.front()->get_globals().at("format").front().declaration);
	const ExpressionInfo    &ei_direct  = cp_program.expressions.at(cs_format.parameters[1].defaultValue.get());
	ASSERT_TRUE(ei_direct.provider);
	EXPECT_EQ(ei_direct.provider->kind, ProviderKind::Format);
	EXPECT_FALSE(ei_direct.provider->callsFormatter);
	ASSERT_EQ(cp_program.textConversions.size(), 1u);
	EXPECT_FALSE(cp_program.textConversions.begin()->second.callsFormatter);
	EXPECT_FALSE(cp_program.defaultEffects.at(&cs_format.parameters[1]).mayCallOrMutate);
	EXPECT_FALSE(cp_program.defaultEffects.at(&cs_format.parameters[2]).mayCallOrMutate);
	expectError(s_program + "fn materialize<T>(T value) { Text<T>? text = null }\nfn start() { materialize(1) }", Code::INVALID_PARAMETER_DEFAULT);
}

TEST (SemanticValues, ReadOnlyFormattersRequireAClassSatisfyingEveryBound) {
	const std::string s_program = R"(typedef<interface> Tagged {}
typedef<class> Base(int id) { pub Base() : (0) {}
    pub operator ::str() = "base" }
fn format<T: Base & Tagged>(T item, opt<string> s ?: item::str) {}
)";
	EXPECT_NO_THROW(checkSource(s_program + "typedef<class> Other : Tagged {}"));
	expectError(s_program + "typedef<class> Both : Base, Tagged {}", Code::INVALID_PARAMETER_DEFAULT);
}

TEST (SemanticValues, ReportsAnEarlierFormatterBeforeAnotherErrorInTheSameDefault) {
	const std::string s_program = R"(typedef<interface> View<T> {}
typedef<class> Text<T> : View<T> { pub operator ::str() = "text" }
fn format(View<int> item, opt<int> n ?: if (true) { item::str; false + 1 } else { 0 }) {}
)";
	expectError(s_program, Code::BINARY_OPERANDS);
	expectError(s_program + "fn later<T>() { Text<int>? text = null }", Code::INVALID_PARAMETER_DEFAULT);
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
