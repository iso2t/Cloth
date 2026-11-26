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

const std::filesystem::path STANDARD_LIBRARY = std::filesystem::path(__FILE__).parent_path() / "../../../../Stdlib/StandardLibrary/src";

CheckedProgram checkPrimitiveSource (const std::string &source, const std::filesystem::path &library = STANDARD_LIBRARY, TargetLayout layout = {}) {
	Lexer lexer{ SourceFile("primitive-client.co", source) };
	return CheckedProgram(SymbolTable(Parser(lexer).parse()), layout, library);
}

void expectPrimitiveError (const std::string &source, Code code, const std::filesystem::path &library = STANDARD_LIBRARY) {
	SCOPED_TRACE(source);
	try {
		checkPrimitiveSource(source, library);
		FAIL() << "Expected a primitive diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.getCode(), code) << error.what();
		ASSERT_FALSE(error.getSourceLabels().empty());
		ASSERT_TRUE(error.getSourceLabels().front().span.source);
		EXPECT_EQ(error.getSourceLabels().front().span.source->getPath(), "primitive-client.co");
	}
}

CheckedProgram checkConsoleSource (const std::string &source) {
	return checkPrimitiveSource("using cloth.Console : print, println, input;\n" + source);
}

void expectConsoleError (const std::string &source, Code code, std::string_view message = {}, std::string_view operand = {}) {
	SCOPED_TRACE(source);
	try {
		checkConsoleSource(source);
		FAIL() << "Expected a console call diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.getCode(), code) << error.what();
		ASSERT_FALSE(error.getSourceLabels().empty());
		const auto &span = error.getSourceLabels().front().span;
		ASSERT_TRUE(span.source);
		EXPECT_EQ(span.source->getPath(), "primitive-client.co");
		if (!message.empty()) {
			EXPECT_EQ(std::string_view(error.what()), message);
			ASSERT_EQ(span.startLine, 2u);
			ASSERT_EQ(span.endLine, span.startLine);
			EXPECT_EQ(source.substr(span.startCol - 1, span.endCol - span.startCol), operand);
		}
	}
}

TEST (BuiltinCalls, NumericLibraryCallsBindDeclaredMethodsAtTheReceiverType) {
	const CheckedProgram program = checkPrimitiveSource(R"(
fn<int<12>> magnitude(int<12> x) = x.abs();
fn<float<16>> half(float<16> x) = x.abs();
fn<float<64>> bounded(float<64> x) = x.clamp(-2.0, 2.0);
fn<usize> count(unsigned int<129> x) = x.trailingZeros();
fn<string> format(float x) = x::str;
)");
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

TEST (BuiltinCalls, TextLibraryCallsBindDeclaredMethodsAndSplitOverloads) {
	const CheckedProgram program = checkPrimitiveSource(R"(
fn<int> flag(bool b) = b.toInt();
fn<int> scalar(char c) = c.toInt();
fn<bool> alnum(char c) = c.isLetterOrDigit();
fn<string> trim(string s) = s.trim();
fn<string[]> byChar(string s) = s.split(',');
fn<string[]> byText(string s) = s.split(", ");
fn<string> format(char c) = c::str;
)");
	for (const auto name : { "flag", "scalar", "alnum", "trim", "byChar", "byText", "format" }) {
		const auto &call = resultOf(program, name);
		EXPECT_FALSE(call.builtin) << name;
		ASSERT_NE(call.symbol, nullptr) << name;
		ASSERT_NE(call.instance, nullptr) << name;
	}
	EXPECT_FALSE(program.declarations.at(resultOf(program, "flag").symbol).intrinsic);
	EXPECT_FALSE(program.declarations.at(resultOf(program, "alnum").symbol).intrinsic);
	EXPECT_FALSE(program.declarations.at(resultOf(program, "byChar").symbol).intrinsic);
	EXPECT_EQ(program.declarations.at(resultOf(program, "byText").symbol).intrinsic, LibraryIntrinsic::StringSplit);
	EXPECT_EQ(program.declarations.at(resultOf(program, "scalar").symbol).intrinsic, LibraryIntrinsic::CharCodePoint);
}

TEST (BuiltinCalls, ConsoleCallsBindLibraryOverloadsAndLocalFunctionsStillTakePrecedence) {
	const CheckedProgram program = checkPrimitiveSource(R"(
using cloth.Console : println, input;
fn<int> print(int n) = n;
fn<int> local() = print(n = 3);
fn show(char? value) = println(value);
fn blank() = println();
fn<string?> read(int n) = input(n);
fn<string?> silent() = input(null);
)");
	const auto          &local   = resultOf(program, "local");
	ASSERT_NE(local.symbol, nullptr);
	EXPECT_EQ(program.declarations.at(local.symbol).file, 0u);
	for (const auto name : { "show", "blank", "read", "silent" }) {
		const auto &call = resultOf(program, name);
		SCOPED_TRACE(name);
		EXPECT_FALSE(call.builtin);
		ASSERT_NE(call.symbol, nullptr);
		const auto &decl = program.declarations.at(call.symbol);
		EXPECT_FALSE(decl.intrinsic);
		EXPECT_EQ(program.units.at(decl.file)->getSourceUnit().getSource()->getPath().filename(), "Console.co");
		EXPECT_EQ(call.instance != nullptr, std::string_view(name) != "blank");
	}
}

TEST (BuiltinCalls, ConsoleNamesRequireAnImportInTheCallingModule) {
	const auto unknown = [] (const std::string &source, const std::filesystem::path &library, const char *name) {
		SCOPED_TRACE(source);
		try {
			checkPrimitiveSource(source, library);
			FAIL() << "Expected an unknown console name";
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.getCode(), Code::UnknownName);
			EXPECT_EQ(std::string(error.what()), std::string("Unknown name '") + name + "'");
			ASSERT_FALSE(error.getSourceLabels().empty());
			EXPECT_EQ(error.getSourceLabels().front().span.source->getPath(), "primitive-client.co");
		}
	};
	for (const auto &library : { std::filesystem::path{}, STANDARD_LIBRARY }) {
		unknown("fn f() { print(1); }", library, "print");
		unknown("fn f() { println(); }", library, "println");
		unknown("fn f() { input(); }", library, "input");
	}
	unknown("using cloth.Console : print; fn f() { println(); }", STANDARD_LIBRARY, "println");
}

TEST (BuiltinCalls, PrimitiveMethodsRequireTheirLibraryDeclarations) {
	for (const char *source : {
	             "fn f(int<129> n) { n.abs(); }",
	             "fn f(unsigned int<12> n) { n.leadingZeros(); }",
	             "fn f(usize n) { n.trailingZeros(); }",
	             "fn f(float<16> n) { n.clamp(0.0, 1.0); }",
	             "fn f(string s) { s.split(','); }",
	             "fn f(string? s) { s?.trim(); }",
	             "fn f(char c) { c.isDigit(); }",
	             "fn f(bool b) { b.toInt(); }",
	             "fn f() { exit(0).abs(); }",
	             "fn f() { exit(0).toInt(); }",
	     })
		expectPrimitiveError(source, Code::UnknownMember, {});
	for (const char *type : { "int<65>", "unsigned int<7>", "float<64>", "string", "char", "bool" }) {
		expectPrimitiveError(std::string("fn<string> f(") + type + " x) = x::str;", Code::UnknownProvider, {});
		expectPrimitiveError(std::string("fn<string> f(") + type + R"( x) = #"{x}";)", Code::UnsupportedInterpolationType, {});
	}
	expectPrimitiveError(R"(fn<string> f(int?[] items) = #"{items}";)", Code::UnsupportedInterpolationType, {});
	EXPECT_NO_THROW(checkSource("fn<int> f(int n) = n + int::max;"));
}

TEST (BuiltinCalls, LibraryMethodsKeepDiagnosticCodesMessagesAndSourceSpans) {
	const struct {
		const char *source;
		Code        code;
		const char *message;
		const char *span;
	} cases[] = {
		{ "fn f() { int<129>::min.abs(); }", Code::IntegerAbsOverflow, "The absolute value of the minimum integer exceeds int<129>", "1:24-1:27" },
		{ "fn f(int<12> x) { x.clamp(4, 2); }", Code::InvalidClampBounds, "The minimum clamp bound exceeds the maximum", "1:21-1:26" },
		{ "fn f(int x) {} fn f(string x) {} fn g() { f(2.toStringRadix(36 + 1)); }", Code::InvalidIntegerRadix,
		  "An integer formatting radix must be between 2 and 36", "1:61-1:67" },
		{ "fn f() { (0.0 / 0.0).signum(); }", Code::FloatSignumNan, "NaN has no signum", "1:22-1:28" },
		{ "fn f(float<16> x) { x.clamp(0.0 / 0.0, x); }", Code::InvalidFloatClampBounds, "Floating clamp bounds must be ordered and cannot be NaN",
		  "1:23-1:28" },
		{ "fn f(float<64> x) { x.clamp(2.0, -1.0); }", Code::InvalidFloatClampBounds, "Floating clamp bounds must be ordered and cannot be NaN",
		  "1:23-1:28" },
		{ "fn f(int<8> x, int y) { x.clamp(y, 2); }", Code::InvalidBuiltinArgument, "'clamp' requires int<8>, got int", "1:33-1:34" },
		{ "fn f(int x) { x.clamp(2); }", Code::CallArity, "No overload of 'clamp' takes 1 arguments", "1:17-1:22" },
		{ "fn f(int x) { x.toStringRadix(radix = 16); }", Code::InvalidArgumentBinding, "'toStringRadix' takes positional arguments", "1:31-1:36" },
		{ "fn f(float x) { x.abs<int>(); }", Code::GenericArity, "'abs' takes no type arguments", "1:22-1:27" },
		{ "fn f(string s) { s.split(4); }", Code::InvalidBuiltinArgument, "'split' requires char or string, got int", "1:26-1:27" },
		{ "fn f(string s, int i) { s.charAt(i); }", Code::InvalidBuiltinArgument, "'charAt' requires usize, got int", "1:34-1:35" },
		{ "fn f(string s) { s.substring(1); }", Code::CallArity, "No overload of 'substring' takes 1 arguments", "1:20-1:29" },
		{ "fn f(string s) { s.split(separator = ','); }", Code::InvalidArgumentBinding, "'split' takes positional arguments", "1:26-1:35" },
		{ "fn f(char c) { c.toInt<int>(); }", Code::GenericArity, "'toInt' takes no type arguments", "1:23-1:28" },
		{ "fn f(bool b) { b.toInt(unknown); }", Code::UnknownName, "Unknown name 'unknown'", "1:24-1:31" },
		{ "fn f(string s) { s.replace(4, unknown); }", Code::UnknownName, "Unknown name 'unknown'", "1:31-1:38" },
		{ "fn f(string? s) { s.trim(); }", Code::UnknownMember, "Unknown method 'trim' on string?", "1:21-1:25" },
		{ "fn f(char? c) { c.isDigit(); }", Code::UnknownMember, "Unknown method 'isDigit' on char?", "1:19-1:26" },
		{ "fn f(bool b) { b.abs(); }", Code::UnknownMember, "Unknown method 'abs' on bool", "1:18-1:21" },
		{ "fn f(string s) { s.split(exit(0), unknown); }", Code::UnknownName, "Unknown name 'unknown'", "1:35-1:42" },
	};

	for (const auto &[source, code, message, span] : cases) {
		SCOPED_TRACE(source);
		try {
			checkPrimitiveSource(source);
			FAIL() << "Expected a library method diagnostic";
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.getCode(), code);
			EXPECT_EQ(std::string_view(error.what()), message);
			ASSERT_FALSE(error.getSourceLabels().empty());
			const auto &primary = error.getSourceLabels().front().span;
			ASSERT_TRUE(primary.source);
			EXPECT_EQ(primary.source->getPath(), "primitive-client.co");
			EXPECT_EQ(primary.toString(), span);
		}
	}
}

TEST (BuiltinCalls, ReportsConsoleErrorsAtTheImportedCall) {
	const struct {
		const char *source;
		Code        code;
		const char *message;
		const char *operand;
	} cases[] = {
		{ "fn f() { def x = print; }", Code::FunctionValue, "'print' is a function", "print" },
		{ "fn f() { print = 1; }", Code::ExpectedBinding, "Expected a writable binding", "print" },
		{ "fn f() { input++; }", Code::ExpectedBinding, "Expected a writable binding", "input" },
		{ "fn f() { print(); }", Code::CallArity, "No overload of 'print' takes 0 arguments", "print" },
		{ "fn f() { println(1, 2); }", Code::CallArity, "No overload of 'println' takes 2 arguments", "println" },
		{ "fn f() { input(prompt = 1); }", Code::InvalidArgumentBinding, "'input' takes positional arguments", "prompt" },
		{ "fn f() { print(1, missing); }", Code::UnknownName, "Unknown name 'missing'", "missing" },
		{ "fn f() { println(value = missing); }", Code::UnknownName, "Unknown name 'missing'", "missing" },
		{ "typedef<struct> S {} fn f(S?[] items) { println(items); }", Code::InvalidBuiltinArgument, "'println' requires a printable value, got S?[]",
		  "items" },
		{ "fn noop() {} fn f() { print(noop()); }", Code::VoidValue, "Expected one value", "noop()" },
		{ "fn<int,int> pair() { return 1, 2; } fn f() { println(pair()); }", Code::MultipleResultsInScalar, "Expected one value", "pair()" },
		{ "fn f() { print([]); }", Code::InvalidArray, "Array literal needs an element type", "[]" },
		{ "fn f() { println([null]); }", Code::ExplicitNullableType, "A nullable value requires an explicit destination type", "[null]" },
		{ "fn f() { print(9999999999999999999999999); }", Code::IntegerRange, "Integer literal is outside int", "9999999999999999999999999" },
	};

	for (const auto &[source, code, message, operand] : cases) expectConsoleError(source, code, message, operand);
}

TEST (BuiltinCalls, RecordsExitAndDeclaredMethodOverloads) {
	const CheckedProgram  cp_program = checkPrimitiveSource(R"(
fn<int> stop(unsigned int<129> status) = exit(status, "done");
fn<string[]> parts(string text) = text.split(',');
fn<string[]> words(string text) = text.split(", ");
fn<int<129>> magnitude(int<129> n) = n.abs();
)");
	const ExpressionInfo &ei_exit    = resultOf(cp_program, "stop");
	ASSERT_TRUE(ei_exit.builtin);
	EXPECT_EQ(ei_exit.builtin->operation, BuiltinOperation::Exit);
	EXPECT_EQ(ei_exit.resultKind(), ResultKind::Never);
	EXPECT_EQ(ei_exit.builtin->parameters, (ResultTypes{ ResolvedType(KInteger), ResolvedType(KString) }));
	const CallExpression &ce_exit = dynamic_cast<const CallExpression &>(bodyExpression(cp_program.units.front()->getGlobals().at("stop").front()));
	EXPECT_EQ(cp_program.expressions.at(ce_exit.suppliedArguments.front().expression.get()).types.front().spelling(), "unsigned int<129>");
	for (const auto &[name, parameter] : { std::pair{ "parts", KCharacter }, std::pair{ "words", KString } }) {
		const auto &call = resultOf(cp_program, name);
		ASSERT_NE(call.symbol, nullptr);
		EXPECT_FALSE(call.builtin);
		EXPECT_EQ(cp_program.declarations.at(call.symbol).parameters, ResultTypes{ ResolvedType(parameter) });
	}
	const auto &abs = resultOf(cp_program, "magnitude");
	ASSERT_NE(abs.symbol, nullptr);
	EXPECT_FALSE(abs.builtin);
	EXPECT_EQ(abs.types.front().spelling(), "int<129>");
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
	for (unsigned const u_bits : { 32u, 64u }) EXPECT_NO_THROW(checkPrimitiveSource(s_source, STANDARD_LIBRARY, { u_bits }));
	const std::string s_index = "fn<char> at(string s) = s.charAt(4294967296);";
	EXPECT_NO_THROW(checkPrimitiveSource(s_index, STANDARD_LIBRARY, { 64 }));
	try {
		checkPrimitiveSource(s_index, STANDARD_LIBRARY, { .sizeBits = 32 });
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
	EXPECT_NO_THROW(checkSource("typedef<class> C(pub def<int> print) { pub mt<int> f() = print; }"));
	EXPECT_NO_THROW(checkSource("typedef<class> C { pub mt<int> print(int n) = n;\npub mt<int> f() { print(2); return self.print(1); } }"));
	expectError("fn f(int print) { print(1); }", Code::NotCallable);
	EXPECT_NO_THROW(checkSource("fn println(bool b) {}\nfn f() { println(3); }"));
	expectError("fn println(bool b) {}\nfn f() { println(3.0); }", Code::NoMatchingCall);
	expectError("fn println(bool b) {}\nfn f() { println(); }", Code::CallArity);
	expectError("fn f() { def x = print; }", Code::UnknownName);
	expectError("fn f() { print = 1; }", Code::UnknownName);
	expectError("fn f() { input++; }", Code::UnknownName);
	expectError("fn exit(int i) {}\nfn<int> f() { exit(1); }", Code::MissingReturn);
}

TEST (BuiltinCalls, ReportTheInvalidOperandOrCallContract) {
	expectConsoleError("fn f() { print(); }", Code::CallArity);
	expectConsoleError("fn f() { println(status = 1); }", Code::InvalidArgumentBinding);
	expectError("fn f() { exit<void>(2); }", Code::InvalidGenericArgument);
	expectPrimitiveError("fn f() { true.toInt<int>(); }", Code::GenericArity);
	expectPrimitiveError("fn f() { true.toInt<int>(missing); }", Code::UnknownName);
	expectConsoleError("fn f() { print(println()); }", Code::VoidValue);
	expectConsoleError("fn f() { print(4294967296); }", Code::IntegerRange);
	expectConsoleError("typedef<struct> S {}\nfn f(S[] s) { print(s); }", Code::InvalidBuiltinArgument);
	expectError("fn f() { exit(true); }", Code::InvalidBuiltinArgument);
	expectError("fn f() { exit(1, null); }", Code::InvalidBuiltinArgument);
	expectConsoleError("fn f() { def line = input(); }", Code::ExplicitNullableType);
	expectPrimitiveError("fn f() { \"a\".split(4); }", Code::InvalidBuiltinArgument);
	expectPrimitiveError("fn f(int i) { \"a\".charAt(i); }", Code::InvalidBuiltinArgument);
	expectPrimitiveError("fn f(int<8> i, int j) { i.clamp(j, 2); }", Code::InvalidBuiltinArgument);
	expectPrimitiveError("fn f(string? s) { s.trim(); }", Code::UnknownMember);
	expectPrimitiveError("fn<string> bad(int? n) = n::str;", Code::NullablePayloadAccess);
	expectPrimitiveError("fn f() { true.ToInt(); }", Code::UnknownMember);
	expectError("fn f(int[] items) { items.append<int>(1); }", Code::GenericArity);
}

TEST (BuiltinCalls, ValidateConstantNumericPreconditionsAtTheReceiverWidth) {
	expectPrimitiveError("fn f() { int<129>::min.abs(); }", Code::IntegerAbsOverflow);
	expectPrimitiveError("fn f(int<129> i) { i.clamp(-2, -3); }", Code::InvalidClampBounds);
	expectPrimitiveError("fn f(int i) {}\nfn f(string s) {}\nfn g() { f(2.toStringRadix(36 + 1)); }", Code::InvalidIntegerRadix);
	expectPrimitiveError("fn f(int i) {}\nfn f(bool b) {}\nfn g() { f(2.toStringRadix(1)); }", Code::InvalidIntegerRadix);
	expectPrimitiveError("fn f() { (0.0 / 0.0).signum(); }", Code::FloatSignumNan);
	expectPrimitiveError("fn f(float<16> v) { v.clamp(0.0 / 0.0, v); }", Code::InvalidFloatClampBounds);
	expectPrimitiveError("fn f(float v) { v.clamp(1.0, -1.0); }", Code::InvalidFloatClampBounds);
	EXPECT_NO_THROW(
	        checkPrimitiveSource("fn f(int i, float v) { i.toStringRadix(i); v.signum(); v.clamp(0.0, 1.0 / 0.0); unsigned int<129>::min.abs(); }"));
}

TEST (BuiltinCalls, CheckUnreachableOperandsAndCaptureTheReceiverBeforeArguments) {
	EXPECT_NO_THROW(checkPrimitiveSource("fn<int> f(int<129> upper) { exit(0).clamp(0, upper); }"));
	EXPECT_NO_THROW(checkPrimitiveSource("fn<int> f() { \"a\".contains(exit(0)).signum(); }"));
	expectPrimitiveError("fn f() { exit(0).abs(missing); }", Code::UnknownName);
	expectPrimitiveError("fn f() { exit(0).unknown(); }", Code::UnknownMember);
	expectError("fn f() { exit(exit(0), true); }", Code::InvalidBuiltinArgument);
	const std::string s_capture =
	        "fn f() { string? text = \"abc\"; string clear = { text = null; return \"b\"; }; if (text != null) { text.contains(clear); ";
	EXPECT_NO_THROW(checkPrimitiveSource(s_capture + "} }"));
	expectPrimitiveError(s_capture + "text.trim(); } }", Code::UnknownMember);
	EXPECT_NO_THROW(checkPrimitiveSource("fn f(int n, opt<int> magnitude ?: n.abs()) {}"));
	const std::string s_default = "\nfn f(C c, opt<int> n ?: if (true) { println(c); 0; } else { 1; }) {}";
	EXPECT_NO_THROW(checkConsoleSource("typedef<class> C {}" + s_default));
	expectConsoleError("typedef<class> C { operator ::str() -> string = \"c\"; }" + s_default, Code::InvalidParameterDefault);
	expectConsoleError("typedef<class> C { pub typedef<class> Child : C() { operator ::str() -> string = \"child\"; } }" + s_default,
	                   Code::InvalidParameterDefault);
	expectConsoleError("typedef<class> C {}\nfn f(C c) { c::delete; print(c); }", Code::DeletedClassAccess);
}
}
}
