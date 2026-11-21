// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>
#include <backend/lowered_module.hpp>

#include <cstddef>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cde/cloth_diagnostic.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Object.h>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>

namespace {

using Code = cde::DiagnosticCode;

cloth::CheckedProgram checkProgram (std::string source, cloth::TargetLayout layout = {}, cloth::ProgramKind kind = cloth::ProgramKind::Executable) {
	cloth::Lexer l_lexer{ cloth::SourceFile("entry.co", std::move(source)) };
	return cloth::CheckedProgram(cloth::SymbolTable(cloth::Parser(l_lexer).parse()), layout, {}, kind);
}

void expectReturn (const cloth::CheckedProgram &program, unsigned expected) {
	const backend::LoweredModule lm_module(program);
	const LLVMModuleRef          mod_module = lm_module.get();
	ASSERT_EQ(LLVMVerifyModule(mod_module, LLVMReturnStatusAction, nullptr), 0);
	const LLVMValueRef fn_main = LLVMGetNamedFunction(mod_module, "main");
	ASSERT_NE(fn_main, nullptr);
	EXPECT_EQ(LLVMGetFirstFunction(mod_module), fn_main);
	EXPECT_EQ(LLVMGetNextFunction(fn_main), nullptr);
	EXPECT_EQ(LLVMGetLinkage(fn_main), LLVMExternalLinkage);
	EXPECT_EQ(LLVMGetFunctionCallConv(fn_main), LLVMCCallConv);
	const LLVMTypeRef ty_function = LLVMGlobalGetValueType(fn_main);
	EXPECT_EQ(LLVMCountParamTypes(ty_function), 0u);
	EXPECT_FALSE(LLVMIsFunctionVarArg(ty_function));
	EXPECT_EQ(LLVMGetReturnType(ty_function), LLVMInt32TypeInContext(LLVMGetModuleContext(mod_module)));
	ASSERT_EQ(LLVMCountBasicBlocks(fn_main), 1u);
	const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_main);
	const LLVMValueRef      ir_ret   = LLVMGetBasicBlockTerminator(bb_entry);
	ASSERT_NE(ir_ret, nullptr);
	EXPECT_EQ(LLVMGetFirstInstruction(bb_entry), ir_ret);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_ret), LLVMRet);
	ASSERT_EQ(LLVMGetNumOperands(ir_ret), 1);
	const LLVMValueRef val_result = LLVMGetOperand(ir_ret, 0);
	ASSERT_NE(LLVMIsAConstantInt(val_result), nullptr);
	EXPECT_EQ(LLVMConstIntGetZExtValue(val_result), expected);
}

TEST (BackendLowering, ReturnsTheLiteralFromTheSelectedEntryOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("pub fn<int> main () = 42", { u_bits }), 42);
		expectReturn(checkProgram("fn<int> helper() { return 99 }\nfn<int<32>> main() = 0x7", { u_bits }), 7);
		expectReturn(checkProgram("fn<int> main() = 0", { u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = 2147483647", { u_bits }), 2147483647);
	}
}

TEST (BackendLowering, ReadsTheLiteralValueFromCheckedFacts) {
	cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42");
	ASSERT_EQ(cp_program.expressions.size(), 1u);
	cp_program.expressions.begin()->second.numericConstant = cloth::IntegerConstant{ { 7 } };
	expectReturn(cp_program, 7);
}

void expectUnsupported (const std::string &source, Code code, std::string_view markedText) {
	SCOPED_TRACE(source);
	const cloth::CheckedProgram cp_program = checkProgram(source);
	try {
		const backend::LoweredModule lm_module(cp_program);
		FAIL() << "Expected a lowering diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), code) << error.what();
		ASSERT_EQ(error.get_source_labels().size(), 1u);
		const cloth::TokenSpan &ts_span = error.get_source_labels().front().span;
		ASSERT_NE(ts_span.source, nullptr);
		EXPECT_EQ(ts_span.source->get_path(), "entry.co");
		ASSERT_EQ(ts_span.startLine, 1u);
		ASSERT_EQ(ts_span.endLine, 1u);
		EXPECT_EQ(source.substr(ts_span.startCol - 1, ts_span.endCol - ts_span.startCol), markedText);
	}
}

TEST (BackendLowering, LabelsUnsupportedBodiesAndExpressions) {
	expectUnsupported("pub fn<int> main() { return 42 }", Code::UNSUPPORTED_LOWERING, "{ return 42 }");
	expectUnsupported("pub fn<int> main() = 40 + 2", Code::UNSUPPORTED_LOWERING, "40 + 2");
	expectUnsupported("pub fn<int> main() = -42", Code::UNSUPPORTED_LOWERING, "-42");
	expectUnsupported("pub fn<int> main() = sideEffect()\nfn<int> sideEffect() { println(1)\nreturn 42 }", Code::UNSUPPORTED_LOWERING, "sideEffect()");
}

TEST (BackendLowering, LabelsUnsupportedEntrySignatures) {
	expectUnsupported("fn<int> main(string[] args) = 42", Code::UNSUPPORTED_FUNCTION_ABI, "main");
	expectUnsupported("fn<int<12>> main() = 42", Code::UNSUPPORTED_FUNCTION_ABI, "main");
	expectUnsupported("fn<isize> main() = 42", Code::UNSUPPORTED_FUNCTION_ABI, "main");
	expectUnsupported("fn<void> main() {}", Code::UNSUPPORTED_FUNCTION_ABI, "main");
}

TEST (BackendLowering, RequiresAnEntrySelectedByTheFrontend) {
	const cloth::CheckedProgram cp_library = checkProgram("pub fn<int> main () = 42", {}, cloth::ProgramKind::Library);
	ASSERT_EQ(cp_library.entryPoint, nullptr);
	try {
		const backend::LoweredModule lm_module(cp_library);
		FAIL() << "Expected an executable compilation request";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), Code::INVALID_COMPILATION_REQUEST) << error.what();
	}
}

TEST (BackendObjectEmission, IndependentCompilationsReturnOwnedObjectBytes) {
	const cloth::CheckedProgram         cp_first  = checkProgram("pub fn<int> main () = 42");
	const cloth::CheckedProgram         cp_second = checkProgram("pub fn<int> main () = 7");
	std::future<std::vector<std::byte>> f_first   = std::async(std::launch::async, [&] { return backend::emitObject(cp_first); });
	const std::vector<std::byte>        v_second  = backend::emitObject(cp_second);
	const std::vector<std::byte>        v_first   = f_first.get();
	ASSERT_FALSE(v_first.empty());
	ASSERT_FALSE(v_second.empty());
	EXPECT_NE(v_first, v_second);
}

template <typename Operation> void expectBackendError (Code code, std::string_view detail, Operation operation) {
	try {
		operation();
		FAIL() << "Expected a backend diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), code) << error.what();
		EXPECT_NE(error.get_message().find(detail), std::string::npos) << error.what();
	}
}

TEST (BackendObjectEmission, RejectsUnknownTargetsAndEmbeddedNulls) {
	const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42");
	expectBackendError(Code::LLVM_TARGET, "cloth-unknown", [&] { static_cast<void>(backend::emitObject(cp_program, "cloth-unknown-none")); });
	const char s_triple[] = "x86_64-linux-gnu\0unused";
	expectBackendError(Code::LLVM_TARGET, "null byte",
	                   [&] { static_cast<void>(backend::emitObject(cp_program, std::string_view(s_triple, sizeof(s_triple) - 1))); });
}

TEST (BackendObjectEmission, ReportsVerifierFailures) {
	const backend::LoweredModule lm_module(checkProgram("pub fn<int> main () = 42"));
	const LLVMValueRef           fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
	LLVMInstructionEraseFromParent(LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main)));
	expectBackendError(Code::INVALID_IR, "terminator", [&] { lm_module.verify(); });
}

#if defined(__i386__) || defined(__x86_64__)
void expectObject (const std::vector<std::byte> &bytes, LLVMBinaryType format, const char *entrySymbol) {
	ASSERT_FALSE(bytes.empty());
	const std::unique_ptr<LLVMOpaqueMemoryBuffer, decltype(&LLVMDisposeMemoryBuffer)> mb_buffer(
	        LLVMCreateMemoryBufferWithMemoryRange(reinterpret_cast<const char *>(bytes.data()), bytes.size(), "cloth", false), LLVMDisposeMemoryBuffer);
	char                                                                 *s_error = nullptr;
	const std::unique_ptr<LLVMOpaqueBinary, decltype(&LLVMDisposeBinary)> bin_object(LLVMCreateBinary(mb_buffer.get(), nullptr, &s_error),
	                                                                                 LLVMDisposeBinary);
	const std::unique_ptr<char, decltype(&LLVMDisposeMessage)>            msg_error(s_error, LLVMDisposeMessage);
	ASSERT_NE(bin_object, nullptr) << (msg_error ? msg_error.get() : "");
	EXPECT_EQ(LLVMBinaryGetType(bin_object.get()), format);
	const std::unique_ptr<LLVMOpaqueSymbolIterator, decltype(&LLVMDisposeSymbolIterator)> it_symbol(LLVMObjectFileCopySymbolIterator(bin_object.get()),
	                                                                                                LLVMDisposeSymbolIterator);
	ASSERT_NE(it_symbol, nullptr);
	for (; !LLVMObjectFileIsSymbolIteratorAtEnd(bin_object.get(), it_symbol.get()); LLVMMoveToNextSymbol(it_symbol.get())) {
		const char *s_name = LLVMGetSymbolName(it_symbol.get());
		if (s_name && std::strcmp(s_name, entrySymbol) == 0) {
			const std::unique_ptr<LLVMOpaqueSectionIterator, decltype(&LLVMDisposeSectionIterator)> it_section(
			        LLVMObjectFileCopySectionIterator(bin_object.get()), LLVMDisposeSectionIterator);
			ASSERT_NE(it_section, nullptr);
			LLVMMoveToContainingSection(it_section.get(), it_symbol.get());
			EXPECT_FALSE(LLVMObjectFileIsSectionIteratorAtEnd(bin_object.get(), it_section.get()));
			return;
		}
	}
	FAIL() << "Missing entry symbol " << entrySymbol;
}

struct TargetCase {
	const char         *triple;
	cloth::TargetLayout layout;
	LLVMBinaryType      format;
	const char         *symbol;
};

TEST (BackendObjectEmission, EmitsCoffAndElfForBothX86Widths) {
	const TargetCase v_targets[] = { { "i686-w64-windows-gnu", { 32, 8 }, LLVMBinaryTypeCOFF, "_main" },
		                         { "x86_64-w64-windows-gnu", { 64, 8 }, LLVMBinaryTypeCOFF, "main" },
		                         { "i386-unknown-linux-gnu", { 32, 4 }, LLVMBinaryTypeELF32L, "main" },
		                         { "x86_64-unknown-linux-gnu", { 64, 4 }, LLVMBinaryTypeELF64L, "main" } };
	for (const TargetCase &tc_target : v_targets) {
		SCOPED_TRACE(tc_target.triple);
		const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42", tc_target.layout);
		expectObject(backend::emitObject(cp_program, tc_target.triple), tc_target.format, tc_target.symbol);
#if defined(_WIN32)
		if (tc_target.format == LLVMBinaryTypeCOFF) expectObject(backend::emitObject(cp_program), tc_target.format, tc_target.symbol);
#elif defined(__linux__)
		if (tc_target.format != LLVMBinaryTypeCOFF) expectObject(backend::emitObject(cp_program), tc_target.format, tc_target.symbol);
#endif
	}
}

TEST (BackendObjectEmission, RejectsPointerWidthAndIntegerAlignmentMismatches) {
	const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42", { 32, 4 });
	expectBackendError(Code::ABI_LAYOUT, "64-bit pointers", [&] { static_cast<void>(backend::emitObject(cp_program, "x86_64-unknown-linux-gnu")); });
	expectBackendError(Code::ABI_LAYOUT, "i64", [&] { static_cast<void>(backend::emitObject(cp_program, "i686-w64-windows-gnu")); });
}
#endif

}
