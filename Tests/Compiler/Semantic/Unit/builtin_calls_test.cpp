// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <string_view>
#include <ast/expr/expressions.hpp>

#include "cde/cloth_diagnostic.hpp"
#include "cde/diagnostic_code.hpp"
#include "checked_program_test.hpp"
#include "gtest/gtest.h"
#include "semantic/checked_program.hpp"
#include "token/token.hpp"

namespace cloth::test {
namespace {
using Code = cde::DiagnosticCode;

const ExpressionInfo &resultOf (const CheckedProgram &program, std::string_view function) {
	return program.expressions.at(&bodyExpression(program.units.front()->getGlobals().at(std::string(function)).front()));
}

const std::filesystem::path NUMERIC_LIBRARY = std::filesystem::path(__FILE__).parent_path() / "../../../../Stdlib/StandardLibrary/src";

CheckedProgram checkNumericSource (const std::string &source, const std::filesystem::path &library) {
	Lexer lexer{ SourceFile("numeric-client.co", source) };
	return CheckedProgram(SymbolTable(Parser(lexer).parse()), {}, library);
}

TEST (BuiltinCalls, NumericLibraryCallsBindDeclaredMethodsAtTheReceiverType) {
	const CheckedProgram program = checkNumericSource(R"(
fn<int<12>> magnitude(int<12> x) = x.abs();
fn<float<16>> half(float<16> x) = x.abs();
fn<float<64>> bounded(float<64> x) = x.clamp(-2.0, 2.0);
fn<usize> count(unsigned int<129> x) = x.trailingZeros();
fn<string> format(float x) = x::str;
)",
	                                                  NUMERIC_LIBRARY);
	for (const auto name : { "magnitude", "half", "bounded", "count", "format" }) {
		const auto &call = resultOf(program, name);
		EXPECT_FALSE(call.builtin) << name;
		ASSERT_NE(call.symbol, nullptr) << name;
		ASSERT_NE(call.instance, nullptr) << name;
	}
	EXPECT_FALSE(program.declarations.at(resultOf(program, "magnitude").symbol).intrinsic);
	EXPECT_FALSE(program.declarations.at(resultOf(program, "bounded").symbol).intrinsic);
	EXPECT_EQ(program.declarations.at(resultOf(program, "half").symbol).intrinsic, LibraryIntrinsic::FloatAbs);
	EXPECT_EQ(program.declarations.at(resultOf(program, "count").symbol).intrinsic, LibraryIntrinsic::IntegerTrailingZeros);
}

TEST (BuiltinCalls, NumericLibraryDiagnosticsKeepTheStandaloneCodesAndSourceSpans) {
	const auto diagnose = [] (const std::string &source, const std::filesystem::path &library, Code code) {
		try {
			checkNumericSource(source, library);
			ADD_FAILURE() << "Expected a diagnostic for " << source;
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.getCode(), code) << error.what();
			if (!error.getSourceLabels().empty()) return error.getSourceLabels().front().span;
		}
		return TokenSpan{};
	};
	const std::pair<const char *, Code> cases[] = {
		{ "fn f() { int<129>::min.abs(); }", Code::IntegerAbsOverflow },
		{ "fn f(int<12> x) { x.clamp(4, 2); }", Code::InvalidClampBounds },
		{ "fn f(int x) {} fn f(string x) {} fn g() { f(2.toStringRadix(36 + 1)); }", Code::InvalidIntegerRadix },
		{ "fn f() { (0.0 / 0.0).signum(); }", Code::FloatSignumNan },
		{ "fn f(float<16> x) { x.clamp(0.0 / 0.0, x); }", Code::InvalidFloatClampBounds },
		{ "fn f(float<64> x) { x.clamp(2.0, -1.0); }", Code::InvalidFloatClampBounds },
		{ "fn f(int<8> x, int y) { x.clamp(y, 2); }", Code::InvalidBuiltinArgument },
		{ "fn f(int x) { x.clamp(2); }", Code::CallArity },
		{ "fn f(int x) { x.toStringRadix(radix = 16); }", Code::InvalidArgumentBinding },
		{ "fn f(float x) { x.abs<int>(); }", Code::GenericArity },
	};
	for (const auto &[source, code] : cases) {
		SCOPED_TRACE(source);
		const TokenSpan standalone = diagnose(source, {}, code), library = diagnose(source, NUMERIC_LIBRARY, code);
		ASSERT_TRUE(library.source);
		EXPECT_EQ(library.source->getPath(), "numeric-client.co");
		EXPECT_EQ(library.toString(), standalone.toString());
	}
}

TEST (BuiltinCalls, RecordsAdaptersOverloadsAndControlFlow) {
	const CheckedProgram  cp_program = checkSource(R"(
typedef<class> Text<T> { operator ::str() -> string = "text"; }
fn show(Text<int>?[] items) = println(items);
fn newline() = println();
fn<string?> read() = input(null);
fn<int> stop(unsigned int<129> status) = exit(status, "done");
fn<string[]> parts(string text) = text.split(',');
fn<string[]> words(string text) = text.split(", ");
fn<int<129>> magnitude(int<129> n) = n.abs();
)");
	const ExpressionInfo &ei_show    = resultOf(cp_program, "show");
	ASSERT_TRUE(ei_show.builtin);
	EXPECT_EQ(ei_show.builtin->operation, BuiltinOperation::Println);
	EXPECT_FALSE(ei_show.builtin->receiver);
	EXPECT_EQ(ei_show.resultKind(), ResultKind::Empty);
	EXPECT_EQ(ei_show.builtin->parameters, ResultTypes{ ResolvedType(KString) });
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
	EXPECT_EQ(ei_exit.builtin->parameters, (ResultTypes{ ResolvedType(KInteger), ResolvedType(KString) }));
	const CallExpression &ce_exit = dynamic_cast<const CallExpression &>(bodyExpression(cp_program.units.front()->getGlobals().at("stop").front()));
	EXPECT_EQ(cp_program.expressions.at(ce_exit.suppliedArguments.front().expression.get()).types.front().spelling(), "unsigned int<129>");
	const BuiltinCall &bc_split = resultOf(cp_program, "parts").builtin.value();
	EXPECT_EQ(bc_split.operation, BuiltinOperation::Split);
	EXPECT_EQ(bc_split.receiver, ResolvedType(KString));
	EXPECT_EQ(bc_split.parameters, ResultTypes{ ResolvedType(KCharacter) });
	EXPECT_EQ(resultOf(cp_program, "words").builtin.value().parameters, ResultTypes{ ResolvedType(KString) });
	const ExpressionInfo &ei_abs = resultOf(cp_program, "magnitude");
	EXPECT_EQ(ei_abs.builtin.value().receiver, ei_abs.types.front());
	EXPECT_EQ(ei_abs.types.front().width, 129u);
}

TEST (BuiltinCalls, PrimitiveSignaturesUseExactValueAndTargetTypes) {
	const std::string s_source = R"(
fn<bool, bool, bool, bool, bool> predicates(string s) = s.isEmpty(), s.contains("x"), s.startsWith("x"), s.endsWith("x"), s.isBlank();
fn<char, string, usize?, usize?, char[]> indexes(string s, usize i) = s.charAt(i), s.substring(i, 2), s.indexOf("x"), s.lastIndexOf("x"), s.toChars();
fn<string, string> replace(string s) = s.replace("x", "y"), s.replaceFirst("x", "y");
fn<string, string> remove(string s) = s.removePrefix("x"), s.removeSuffix("x");
fn<string, string, string> trim(string s) = s.trim(), s.trimStart(), s.trimEnd();
fn<string, string> letters(string s) = s.toUpperCase(), s.toLowerCase();
fn<bool, bool, bool> charKinds(char c) = c.isWhitespace(), c.isLetter(), c.isDigit();
fn<bool, bool, bool> charCase(char c) = c.isLetterOrDigit(), c.isUpperCase(), c.isLowerCase();
fn<char, char, int, int> convert(char c, bool b) = c.toUpperCase(), c.toLowerCase(), c.toInt(), b.toInt();
fn<usize, usize, string, int, isize> integer(isize n) = n.leadingZeros(), n.trailingZeros(), n.toStringRadix(36), n.signum(), n.abs().clamp(-1, 4);
fn<float<16>, int> floating(float<16> n) = n.abs().clamp(0.0, 1.0), n.signum();
fn<usize?> guarded(string? s) = s?.indexOf("x");
fn<char?> at(string? s) = s?.charAt(1);
)";
	for (unsigned const u_bits : { 32u, 64u }) EXPECT_NO_THROW(checkSource(s_source, { u_bits }));
	const std::string s_index = "fn<char> at(string s) = s.charAt(4294967296);";
	EXPECT_NO_THROW(checkSource(s_index, { 64 }));
	try {
		checkSource(s_index, { .sizeBits=32 });
		FAIL() << "Index exceeds the 32-bit target size";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.getCode(), Code::IntegerRange);
	}
}

TEST (BuiltinCalls, LexicalDeclarationsShadowTheWholeBuiltinGroup) {
	const CheckedProgram  cp_program = checkSource("fn<int> f() = input(2);\nfn<int> input(int code) = code;");
	const ExpressionInfo &ei_input   = resultOf(cp_program, "f");
	EXPECT_FALSE(ei_input.builtin);
	EXPECT_EQ(ei_input.symbol, &cp_program.units.front()->getGlobals().at("input").front());
	EXPECT_NO_THROW(checkSource("typedef<class> C { pub mt<string> abs() = \"ok\"; }\nfn<string> f(C item) = item.abs();"));
	EXPECT_NO_THROW(checkSource("typedef<class> C(pub def<int> print) { pub mt<int> f() { print(self.print); return self.print; } }"));
	EXPECT_NO_THROW(checkSource("typedef<class> C { pub mt<int> print(int n) = n;\npub mt<int> f() { print(\"x\"); return self.print(1); } }"));
	expectError("fn f(int print) { print(1); }", Code::NotCallable);
	EXPECT_NO_THROW(checkSource("fn println(bool b) {}\nfn f() { println(3); }"));
	expectError("fn println(bool b) {}\nfn f() { println(3.0); }", Code::NoMatchingCall);
	expectError("fn println(bool b) {}\nfn f() { println(); }", Code::CallArity);
	expectError("fn f() { def x = print; }", Code::FunctionValue);
	expectError("fn f() { print = 1; }", Code::ExpectedBinding);
	expectError("fn f() { input++; }", Code::ExpectedBinding);
	expectError("fn exit(int i) {}\nfn<int> f() { exit(1); }", Code::MissingReturn);
}

TEST (BuiltinCalls, ReportTheInvalidOperandOrCallContract) {
	expectError("fn f() { print(); }", Code::CallArity);
	expectError("fn f() { println(status = 1); }", Code::InvalidArgumentBinding);
	expectError("fn f() { exit<void>(2); }", Code::InvalidGenericArgument);
	expectError("fn f() { true.toInt<int>(); }", Code::GenericArity);
	expectError("fn f() { true.toInt<int>(missing); }", Code::UnknownName);
	expectError("fn f() { print(println()); }", Code::VoidValue);
	expectError("fn f() { print(4294967296); }", Code::IntegerRange);
	expectError("typedef<struct> S {}\nfn f(S[] s) { print(s); }", Code::InvalidBuiltinArgument);
	expectError("fn f() { exit(true); }", Code::InvalidBuiltinArgument);
	expectError("fn f() { exit(1, null); }", Code::InvalidBuiltinArgument);
	expectError("fn f() { input(1); }", Code::InvalidBuiltinArgument);
	expectError("fn f() { def line = input(); }", Code::ExplicitNullableType);
	expectError("fn f() { \"a\".split(4); }", Code::InvalidBuiltinArgument);
	expectError("fn f(int i) { \"a\".charAt(i); }", Code::InvalidBuiltinArgument);
	expectError("fn f(int<8> i, int j) { i.clamp(j, 2); }", Code::InvalidBuiltinArgument);
	expectError("fn f(string? s) { s.trim(); }", Code::UnknownMember);
	expectError("fn f() { true.ToInt(); }", Code::UnknownMember);
	expectError("fn f(int[] items) { items.append<int>(1); }", Code::GenericArity);
}

TEST (BuiltinCalls, ValidateConstantNumericPreconditionsAtTheReceiverWidth) {
	expectError("fn f() { int<129>::min.abs(); }", Code::IntegerAbsOverflow);
	expectError("fn f(int<129> i) { i.clamp(-2, -3); }", Code::InvalidClampBounds);
	expectError("fn f(int i) {}\nfn f(string s) {}\nfn g() { f(2.toStringRadix(36 + 1)); }", Code::InvalidIntegerRadix);
	expectError("fn f(int i) {}\nfn f(bool b) {}\nfn g() { f(2.toStringRadix(1)); }", Code::InvalidIntegerRadix);
	expectError("fn f() { (0.0 / 0.0).signum(); }", Code::FloatSignumNan);
	expectError("fn f(float<16> v) { v.clamp(0.0 / 0.0, v); }", Code::InvalidFloatClampBounds);
	expectError("fn f(float v) { v.clamp(1.0, -1.0); }", Code::InvalidFloatClampBounds);
	EXPECT_NO_THROW(checkSource("fn f(int i, float v) { i.toStringRadix(i); v.signum(); v.clamp(0.0, 1.0 / 0.0); unsigned int<129>::min.abs(); }"));
}

TEST (BuiltinCalls, CheckUnreachableOperandsAndCaptureTheReceiverBeforeArguments) {
	EXPECT_NO_THROW(checkSource("fn<int> f(int<129> upper) { exit(0).clamp(0, upper); }"));
	EXPECT_NO_THROW(checkSource("fn<int> f() { \"a\".contains(exit(0)).signum(); }"));
	expectError("fn f() { exit(0).abs(missing); }", Code::UnknownName);
	expectError("fn f() { exit(0).unknown(); }", Code::UnknownMember);
	expectError("fn f() { exit(exit(0), true); }", Code::InvalidBuiltinArgument);
	const std::string s_capture =
	        "fn f() { string? text = \"abc\"; string clear = { text = null; return \"b\"; }; if (text != null) { text.contains(clear); ";
	EXPECT_NO_THROW(checkSource(s_capture + "} }"));
	expectError(s_capture + "text.trim(); } }", Code::UnknownMember);
	EXPECT_NO_THROW(checkSource("fn f(int n, opt<int> magnitude ?: n.abs()) {}"));
	const std::string s_default = "\nfn f(C c, opt<int> n ?: if (true) { println(c); 0; } else { 1; }) {}";
	EXPECT_NO_THROW(checkSource("typedef<class> C {}" + s_default));
	expectError("typedef<class> C { operator ::str() -> string = \"c\"; }" + s_default, Code::InvalidParameterDefault);
	expectError("typedef<class> C { pub typedef<class> Child : C() { operator ::str() -> string = \"child\"; } }" + s_default,
	            Code::InvalidParameterDefault);
	expectError("typedef<class> C {}\nfn f(C c) { c::delete; print(c); }", Code::DeletedClassAccess);
}
}
}
