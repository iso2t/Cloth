// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <ast/expr/expressions.hpp>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {
using Code = cde::DiagnosticCode;

const ExpressionInfo &resultOf (const CheckedProgram &program, std::string_view function) {
	return program.expressions.at(&bodyExpression(program.units.front()->get_globals().at(std::string(function)).front()));
}

TEST (BuiltinCalls, RecordsAdaptersOverloadsAndControlFlow) {
	const CheckedProgram  cp_program = checkSource(R"(
typedef<class> Text<T> { operator ::str() -> string = "text" }
fn show(Text<int>?[] items) = println(items)
fn newline() = println()
fn<string?> read() = input(null)
fn<int> stop(unsigned int<129> status) = exit(status, "done")
fn<string[]> parts(string text) = text.split(',')
fn<string[]> words(string text) = text.split(", ")
fn<int<129>> magnitude(int<129> n) = n.abs()
)");
	const ExpressionInfo &ei_show    = resultOf(cp_program, "show");
	ASSERT_TRUE(ei_show.builtin);
	EXPECT_EQ(ei_show.builtin->operation, BuiltinOperation::Println);
	EXPECT_FALSE(ei_show.builtin->receiver);
	EXPECT_EQ(ei_show.resultKind(), ResultKind::Empty);
	EXPECT_EQ(ei_show.builtin->parameters, ResultTypes{ ResolvedType(kString) });
	ASSERT_EQ(cp_program.textConversions.size(), 1u);
	const ProviderSelection &ps_text = cp_program.textConversions.begin()->second;
	EXPECT_TRUE(ps_text.receiver.array());
	EXPECT_TRUE(ps_text.receiver.element().nullable());
	EXPECT_TRUE(ps_text.formatter);
	EXPECT_NE(ps_text.formatterInstance, nullptr);
	EXPECT_TRUE(resultOf(cp_program, "newline").builtin.value().parameters.empty());
	const ExpressionInfo &ei_read = resultOf(cp_program, "read");
	ASSERT_TRUE(ei_read.builtin);
	EXPECT_EQ(ei_read.builtin->operation, BuiltinOperation::Input);
	EXPECT_EQ(ei_read.types, ei_read.builtin->parameters);
	EXPECT_EQ(ei_read.types.front().spelling(), "string?");
	const ExpressionInfo &ei_exit = resultOf(cp_program, "stop");
	ASSERT_TRUE(ei_exit.builtin);
	EXPECT_EQ(ei_exit.builtin->operation, BuiltinOperation::Exit);
	EXPECT_EQ(ei_exit.resultKind(), ResultKind::Never);
	EXPECT_EQ(ei_exit.builtin->parameters, (ResultTypes{ ResolvedType(kInteger), ResolvedType(kString) }));
	const CallExpression &ce_exit = dynamic_cast<const CallExpression &>(bodyExpression(cp_program.units.front()->get_globals().at("stop").front()));
	EXPECT_EQ(cp_program.expressions.at(ce_exit.suppliedArguments.front().expression.get()).types.front().spelling(), "unsigned int<129>");
	const BuiltinCall &bc_split = resultOf(cp_program, "parts").builtin.value();
	EXPECT_EQ(bc_split.operation, BuiltinOperation::Split);
	EXPECT_EQ(bc_split.receiver, ResolvedType(kString));
	EXPECT_EQ(bc_split.parameters, ResultTypes{ ResolvedType(kCharacter) });
	EXPECT_EQ(resultOf(cp_program, "words").builtin.value().parameters, ResultTypes{ ResolvedType(kString) });
	const ExpressionInfo &ei_abs = resultOf(cp_program, "magnitude");
	EXPECT_EQ(ei_abs.builtin.value().receiver, ei_abs.types.front());
	EXPECT_EQ(ei_abs.types.front().width, 129u);
}

TEST (BuiltinCalls, PrimitiveSignaturesUseExactValueAndTargetTypes) {
	const std::string s_source = R"(
fn<bool, bool, bool, bool, bool> predicates(string s) = s.isEmpty(), s.contains("x"), s.startsWith("x"), s.endsWith("x"), s.isBlank()
fn<char, string, usize?, usize?, char[]> indexes(string s, usize i) = s.charAt(i), s.substring(i, 2), s.indexOf("x"), s.lastIndexOf("x"), s.toChars()
fn<string, string> replace(string s) = s.replace("x", "y"), s.replaceFirst("x", "y")
fn<string, string> remove(string s) = s.removePrefix("x"), s.removeSuffix("x")
fn<string, string, string> trim(string s) = s.trim(), s.trimStart(), s.trimEnd()
fn<string, string> letters(string s) = s.toUpperCase(), s.toLowerCase()
fn<bool, bool, bool> charKinds(char c) = c.isWhitespace(), c.isLetter(), c.isDigit()
fn<bool, bool, bool> charCase(char c) = c.isLetterOrDigit(), c.isUpperCase(), c.isLowerCase()
fn<char, char, int, int> convert(char c, bool b) = c.toUpperCase(), c.toLowerCase(), c.toInt(), b.toInt()
fn<usize, usize, string, int, isize> integer(isize n) = n.leadingZeros(), n.trailingZeros(), n.toStringRadix(36), n.signum(), n.abs().clamp(-1, 4)
fn<float<16>, int> floating(float<16> n) = n.abs().clamp(0.0, 1.0), n.signum()
fn<usize?> guarded(string? s) = s?.indexOf("x")
fn<char?> at(string? s) = s?.charAt(1)
)";
	for (unsigned u_bits : { 32u, 64u }) EXPECT_NO_THROW(checkSource(s_source, { u_bits }));
	const std::string s_index = "fn<char> at(string s) = s.charAt(4294967296)";
	EXPECT_NO_THROW(checkSource(s_index, { 64 }));
	try {
		checkSource(s_index, { 32 });
		FAIL() << "Index exceeds the 32-bit target size";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), Code::INTEGER_RANGE);
	}
}

TEST (BuiltinCalls, LexicalDeclarationsShadowTheWholeBuiltinGroup) {
	const CheckedProgram  cp_program = checkSource("fn<int> f() = input(2)\nfn<int> input(int code) = code");
	const ExpressionInfo &ei_input   = resultOf(cp_program, "f");
	EXPECT_FALSE(ei_input.builtin);
	EXPECT_EQ(ei_input.symbol, &cp_program.units.front()->get_globals().at("input").front());
	EXPECT_NO_THROW(checkSource("typedef<class> C { pub mt<string> abs() = \"ok\" }\nfn<string> f(C item) = item.abs()"));
	EXPECT_NO_THROW(checkSource("typedef<class> C(pub def<int> print) { pub mt<int> f() { print(self.print); return self.print } }"));
	EXPECT_NO_THROW(checkSource("typedef<class> C { pub mt<int> print(int n) = n\npub mt<int> f() { print(\"x\"); return self.print(1) } }"));
	expectError("fn f(int print) { print(1) }", Code::NOT_CALLABLE);
	expectError("fn println(bool b) {}\nfn f() { println(3) }", Code::NO_MATCHING_CALL);
	expectError("fn println(bool b) {}\nfn f() { println() }", Code::CALL_ARITY);
	expectError("fn f() { def x = print }", Code::FUNCTION_VALUE);
	expectError("fn f() { print = 1 }", Code::EXPECTED_BINDING);
	expectError("fn f() { input++ }", Code::EXPECTED_BINDING);
	expectError("fn exit(int i) {}\nfn<int> f() { exit(1) }", Code::MISSING_RETURN);
}

TEST (BuiltinCalls, ReportTheInvalidOperandOrCallContract) {
	expectError("fn f() { print() }", Code::CALL_ARITY);
	expectError("fn f() { println(status = 1) }", Code::INVALID_ARGUMENT_BINDING);
	expectError("fn f() { exit<void>(2) }", Code::INVALID_GENERIC_ARGUMENT);
	expectError("fn f() { true.toInt<int>() }", Code::GENERIC_ARITY);
	expectError("fn f() { true.toInt<int>(missing) }", Code::UNKNOWN_NAME);
	expectError("fn f() { print(println()) }", Code::VOID_VALUE);
	expectError("fn f() { print(4294967296) }", Code::INTEGER_RANGE);
	expectError("typedef<struct> S {}\nfn f(S[] s) { print(s) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f() { exit(true) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f() { exit(1, null) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f() { input(1) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f() { def line = input() }", Code::EXPLICIT_NULLABLE_TYPE);
	expectError("fn f() { \"a\".split(4) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f(int i) { \"a\".charAt(i) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f(int<8> i, int j) { i.clamp(j, 2) }", Code::INVALID_BUILTIN_ARGUMENT);
	expectError("fn f(string? s) { s.trim() }", Code::UNKNOWN_MEMBER);
	expectError("fn f() { true.ToInt() }", Code::UNKNOWN_MEMBER);
	expectError("fn f(int[] items) { items.append<int>(1) }", Code::GENERIC_ARITY);
}

TEST (BuiltinCalls, ValidateConstantNumericPreconditionsAtTheReceiverWidth) {
	expectError("fn f() { int<129>::min.abs() }", Code::INTEGER_ABS_OVERFLOW);
	expectError("fn f(int<129> i) { i.clamp(-2, -3) }", Code::INVALID_CLAMP_BOUNDS);
	expectError("fn f(int i) {}\nfn f(string s) {}\nfn g() { f(2.toStringRadix(36 + 1)) }", Code::INVALID_INTEGER_RADIX);
	expectError("fn f(int i) {}\nfn f(bool b) {}\nfn g() { f(2.toStringRadix(1)) }", Code::INVALID_INTEGER_RADIX);
	expectError("fn f() { (0.0 / 0.0).signum() }", Code::FLOAT_SIGNUM_NAN);
	expectError("fn f(float<16> v) { v.clamp(0.0 / 0.0, v) }", Code::INVALID_FLOAT_CLAMP_BOUNDS);
	expectError("fn f(float v) { v.clamp(1.0, -1.0) }", Code::INVALID_FLOAT_CLAMP_BOUNDS);
	EXPECT_NO_THROW(checkSource("fn f(int i, float v) { i.toStringRadix(i); v.signum(); v.clamp(0.0, 1.0 / 0.0); unsigned int<129>::min.abs() }"));
}

TEST (BuiltinCalls, CheckUnreachableOperandsAndCaptureTheReceiverBeforeArguments) {
	EXPECT_NO_THROW(checkSource("fn<int> f(int<129> upper) { exit(0).clamp(0, upper) }"));
	EXPECT_NO_THROW(checkSource("fn<int> f() { \"a\".contains(exit(0)).signum() }"));
	expectError("fn f() { exit(0).abs(missing) }", Code::UNKNOWN_NAME);
	expectError("fn f() { exit(0).unknown() }", Code::UNKNOWN_MEMBER);
	expectError("fn f() { exit(exit(0), true) }", Code::INVALID_BUILTIN_ARGUMENT);
	const std::string s_capture =
	        "fn f() { def<string?> text = \"abc\"; def<string> clear = { text = null; return \"b\" }; if (text != null) { text.contains(clear); ";
	EXPECT_NO_THROW(checkSource(s_capture + "} }"));
	expectError(s_capture + "text.trim() } }", Code::UNKNOWN_MEMBER);
	EXPECT_NO_THROW(checkSource("fn f(int n, opt<int> magnitude ?: n.abs()) {}"));
	const std::string s_default = "\nfn f(C c, opt<int> n ?: if (true) { println(c); 0 } else { 1 }) {}";
	EXPECT_NO_THROW(checkSource("typedef<class> C {}" + s_default));
	expectError("typedef<class> C { operator ::str() -> string = \"c\" }" + s_default, Code::INVALID_PARAMETER_DEFAULT);
	expectError("typedef<class> C { pub typedef<class> Child : C() { operator ::str() -> string = \"child\" } }" + s_default,
	            Code::INVALID_PARAMETER_DEFAULT);
	expectError("typedef<class> C {}\nfn f(C c) { c::delete; print(c) }", Code::DELETED_CLASS_ACCESS);
}
}
}
