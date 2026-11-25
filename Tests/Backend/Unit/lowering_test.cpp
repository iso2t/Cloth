// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>
#include <backend/integer_range.hpp>
#include <backend/lowered_module.hpp>
#include <backend/nullable_value.hpp>
#include <backend/scalar_lowering.hpp>
#include <backend/value_types.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ast/stmt/loop_control.hpp>
#include <diagnostics.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Object.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/program_kind.hpp>
#include <semantic/symbol_table.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

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
		expectReturn(checkProgram("pub fn<int> main () = 42", { .sizeBits=u_bits }), 42);
		expectReturn(checkProgram("pub fn<int> main () {\n    return 42\n}", { .sizeBits=u_bits }), 42);
		expectReturn(checkProgram("fn<int> helper() { return 99 }\nfn<int<32>> main() = 0x7", { .sizeBits=u_bits }), 7);
		expectReturn(checkProgram("fn<int> main() = 0", { .sizeBits=u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = 2147483647", { .sizeBits=u_bits }), 2147483647);
	}
}

TEST (BackendLowering, ReadsTheLiteralValueFromCheckedFacts) {
	for (const char *s_source : { "pub fn<int> main () = 42", "pub fn<int> main () { return 42 }" }) {
		SCOPED_TRACE(s_source);
		cloth::CheckedProgram cp_program = checkProgram(s_source);
		ASSERT_EQ(cp_program.expressions.size(), 1u);
		cp_program.expressions.begin()->second.numericConstant = cloth::IntegerConstant{ .magnitude={ 7 } };
		expectReturn(cp_program, 7);
	}
}

TEST (BackendLowering, ReturnsImmutableIntegerLocalsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("pub fn<int> main() { int answer := 42; return answer }", { .sizeBits=u_bits }), 42);
		expectReturn(checkProgram("pub fn<int> main() { def answer := 7; return answer }", { .sizeBits=u_bits }), 7);
		expectReturn(checkProgram("fn<int> main() { int<32> source := 42; def copy := source; int other := 7; return copy }", { .sizeBits=u_bits }), 42);
		expectReturn(checkProgram("fn<int> main() { int caf\u00e9 := 7; return cafe\u0301 }", { .sizeBits=u_bits }), 7);
	}
}

TEST (BackendLowering, UsesTheEnumCaseSelectedByTheFrontend) {
	std::ostringstream oss_source;
	oss_source << "typedef<enum> Mode { pub Zulu";
	for (unsigned u_index = 1; u_index < 256; ++u_index) oss_source << ", Hidden" << u_index;
	oss_source << ", pub Alpha }\nfn<int> main() = Mode.Zulu -> int";
	cloth::CheckedProgram     cp_program = checkProgram(oss_source.str());
	const cloth::SymbolScope &ss_cases   = cp_program.units.front()->getGlobals().at("Mode").front().members;
	const cloth::Symbol      &sym_zulu = ss_cases.at("Zulu").front(), &sym_alpha = ss_cases.at("Alpha").front();
	EXPECT_EQ(sym_zulu.enumCaseOrdinal, 0u);
	EXPECT_EQ(sym_alpha.enumCaseOrdinal, 256u);
	// Both cases have the same nominal type. Only the selected declaration changes.
	for (cloth::ExpressionFacts::value_type &ef_value : cp_program.expressions) {
		if (ef_value.second.symbol == &sym_zulu) ef_value.second.symbol = &sym_alpha;
	}
	expectReturn(cp_program, 256u);
}

TEST (BackendLowering, StoresAndComparesFullWidthEnumTagsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(typedef<enum> Mode { pub Off, pub On }
typedef<struct> Setting { pub bool enabled, pub Mode mode }
fn<int> main() { Setting s = {true, Mode.On}; return if (different(s.mode, Mode.Off)) 42 else 7 }
fn<bool> different(final Mode a, final Mode b) = a != b)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef     mod_module = lm_module.get();
		const LLVMTypeRef       ty_tag     = LLVMInt32TypeInContext(LLVMGetModuleContext(mod_module));
		const LLVMTargetDataRef td_layout  = LLVMGetModuleDataLayout(mod_module);
		const LLVMValueRef      fn_main    = LLVMGetNamedFunction(mod_module, "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_setting = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_setting), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_setting), 2u);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_setting, 1), ty_tag);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_setting, 1), 4u);
		EXPECT_EQ(LLVMABISizeOfType(td_layout, ty_setting), 8u);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_layout, ty_setting), 4u);

		const LLVMValueRef fn_compare = LLVMGetNamedFunction(mod_module, "cloth.different");
		ASSERT_NE(fn_compare, nullptr);
		const LLVMValueRef ir_test = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_compare));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_test), LLVMICmp);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_test), LLVMIntNE);
		EXPECT_EQ(LLVMTypeOf(LLVMGetOperand(ir_test, 0)), ty_tag);
		EXPECT_EQ(LLVMGetOperand(ir_test, 0), LLVMGetParam(fn_compare, 0));
		EXPECT_EQ(LLVMGetOperand(ir_test, 1), LLVMGetParam(fn_compare, 1));
	}
}

TEST (BackendLowering, IndexesOneReadOnlyTablePerPayloadFieldUsingCheckedCaseValues) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		cloth::CheckedProgram     cp_program = checkProgram(R"(typedef<enum> Mode(def<string> label = "mode", pub def<usize> code = 7) {
    Hidden("hidden", 90), pub Off, pub On("on", 42)
}
fn<int> main() = (Mode.Off.code + read(Mode.On)) -> int
fn<usize> read(final Mode mode) = mode.code)",
		                                                    { .sizeBits=u_bits });
		const cloth::SymbolScope &ss_members = cp_program.units.front()->getGlobals().at("Mode").front().members;
		// Alter the checked selection independently of the declaration's literal.
		cp_program.enumPayloads.at(&ss_members.at("On").front())[1] = cp_program.enumPayloads.at(&ss_members.at("Off").front())[1];
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef ir_table = LLVMGetFirstGlobal(lm_module.get());
		ASSERT_NE(ir_table, nullptr);
		EXPECT_EQ(LLVMGetNextGlobal(ir_table), nullptr);
		EXPECT_EQ(LLVMGetLinkage(ir_table), LLVMPrivateLinkage);
		EXPECT_TRUE(LLVMIsGlobalConstant(ir_table));
		const LLVMTypeRef ty_table = LLVMGlobalGetValueType(ir_table);
		ASSERT_EQ(LLVMGetTypeKind(ty_table), LLVMArrayTypeKind);
		EXPECT_EQ(LLVMGetArrayLength2(ty_table), 3u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMGetElementType(ty_table)), u_bits);
		const LLVMValueRef ir_values = LLVMGetInitializer(ir_table);
		for (unsigned u_index = 0; u_index < 3; ++u_index) {
			const LLVMValueRef ir_value = LLVMGetAggregateElement(ir_values, u_index);
			ASSERT_NE(LLVMIsAConstantInt(ir_value), nullptr);
			EXPECT_EQ(LLVMConstIntGetZExtValue(ir_value), u_index ? 7u : 90u);
		}
		const LLVMValueRef fn_read = LLVMGetNamedFunction(lm_module.get(), "cloth.read");
		ASSERT_NE(fn_read, nullptr);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetParam(fn_read, 0))), 32u);
	}
}

TEST (BackendLowering, NullableScalarsStoreAPresenceByteAndAnAlignedPayload) {
	for (const unsigned u_bits : { 32u, 64u }) {
		for (const std::string s_payload : { "int", "unsigned int<1>", "int<12>", "unsigned int<64>", "int<97>", "usize", "isize", "float<16>", "float",
		                                     "float<64>", "bool", "char", "Mode" }) {
			SCOPED_TRACE(::testing::Message() << s_payload << " on " << u_bits << "-bit target");
			const std::string            s_type       = s_payload + "?";
			const cloth::CheckedProgram  cp_program   = checkProgram("typedef<enum> Mode { pub Off }\n"
			                                                         "fn<int> main() = if (identity(null) == null) 42 else 7\nfn<" +
			                                                                 s_type + "> identity(final " + s_type + " value) = value",
			                                                         { .sizeBits=u_bits });
			const cloth::Symbol         &sym_identity = cp_program.units.front()->getGlobals().at("identity").front();
			const cloth::ResolvedType   &rt_type      = cp_program.declarations.at(&sym_identity).parameters.front();
			const cloth::ResolvedType    rt_payload   = rt_type.element();
			const cloth::StorageLayout   sl_nullable  = cp_program.storageLayout(rt_type).value();
			const cloth::StorageLayout   sl_payload   = cp_program.storageLayout(rt_payload).value();
			const backend::LoweredModule lm_module(cp_program);
			ASSERT_NO_THROW(lm_module.verify());
			const LLVMValueRef fn_identity = LLVMGetNamedFunction(lm_module.get(), "cloth.identity");
			ASSERT_NE(fn_identity, nullptr);
			const LLVMTypeRef       ty_nullable = LLVMTypeOf(LLVMGetParam(fn_identity, 0));
			const LLVMTargetDataRef td_layout   = LLVMGetModuleDataLayout(lm_module.get());
			ASSERT_EQ(LLVMGetTypeKind(ty_nullable), LLVMStructTypeKind);
			ASSERT_EQ(LLVMCountStructElementTypes(ty_nullable), 2u);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_nullable, 0)), 8u);
			const LLVMTypeRef ty_payload = LLVMStructGetTypeAtIndex(ty_nullable, 1);
			EXPECT_EQ(LLVMGetTypeKind(ty_payload) == LLVMIntegerTypeKind, !rt_payload.floating());
			EXPECT_EQ(LLVMSizeOfTypeInBits(td_layout, ty_payload), rt_payload.width ? rt_payload.width : sl_payload.size * CHAR_BIT);
			EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_nullable, 0), 0u);
			EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_nullable, 1), sl_payload.alignment);
			EXPECT_EQ(LLVMABISizeOfType(td_layout, ty_nullable), sl_nullable.size);
			EXPECT_EQ(LLVMABIAlignmentOfType(td_layout, ty_nullable), sl_nullable.alignment);
			EXPECT_EQ(LLVMGetReturnType(LLVMGlobalGetValueType(fn_identity)), ty_nullable);
		}
	}
}

TEST (BackendLowering, NullableBooleansStorePresenceAndTruthInSeparateBytes) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program =
		        checkProgram(R"(typedef<enum> Signal(pub def<bool?> flag = null) { pub Missing, pub Off(0), pub On(256 -> int?) }
fn<int> main() = if (identity(Signal.On.flag) == true) 42 else 7
fn<bool?> identity(final bool? flag) = flag)",
		                     { .sizeBits=u_bits });
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_identity = LLVMGetNamedFunction(lm_module.get(), "cloth.identity");
		ASSERT_NE(fn_identity, nullptr);
		const LLVMTypeRef       ty_nullable = LLVMTypeOf(LLVMGetParam(fn_identity, 0));
		const LLVMTypeRef       ty_byte     = LLVMInt8TypeInContext(LLVMGetModuleContext(lm_module.get()));
		const LLVMTargetDataRef td_layout   = LLVMGetModuleDataLayout(lm_module.get());
		ASSERT_EQ(LLVMGetTypeKind(ty_nullable), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_nullable), 2u);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_nullable, 0), ty_byte);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_nullable, 1), ty_byte);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_nullable, 1), 1u);
		EXPECT_EQ(LLVMABISizeOfType(td_layout, ty_nullable), 2u);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_layout, ty_nullable), 1u);
		EXPECT_EQ(LLVMGetReturnType(LLVMGlobalGetValueType(fn_identity)), ty_nullable);

		const LLVMValueRef ir_table = LLVMGetFirstGlobal(lm_module.get());
		ASSERT_NE(ir_table, nullptr);
		const LLVMTypeRef ty_table = LLVMGlobalGetValueType(ir_table);
		ASSERT_EQ(LLVMGetTypeKind(ty_table), LLVMArrayTypeKind);
		ASSERT_EQ(LLVMGetElementType(ty_table), ty_nullable);
		ASSERT_EQ(LLVMGetArrayLength2(ty_table), 3u);
		for (unsigned u_case = 0; u_case < 3; ++u_case) {
			const LLVMValueRef ir_flag = LLVMGetAggregateElement(LLVMGetInitializer(ir_table), u_case);
			ASSERT_NE(ir_flag, nullptr);
			EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_flag, 0)), u_case != 0);
			EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_flag, 1)), u_case == 2);
		}
	}
}

TEST (BackendLowering, UsesCheckedStorageForImmutableStructFieldsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(typedef<struct> Sample<T> { pub bool ready, pub T value }
pub fn<int> main() = small(true, 42) + sized(true, 0)
fn<int> small(final bool flag, final int<12> n) {
    Sample<int<12>> sample := {flag, n}
    return if (sample.ready) sample.value -> int else 0
}
fn<int> sized(final bool flag, final usize n) {
    Sample<usize> sample := {flag, n}
    return if (sample.ready) sample.value -> int else 0
})",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());

		struct FieldCase {
			const char *function;
			unsigned    width, offset, size, alignment;
		};

		const unsigned u_word = u_bits / 8;
		for (const FieldCase fc_case : { FieldCase{ .function="cloth.small", .width=12, .offset=2, .size=4, .alignment=2 }, FieldCase{ .function="cloth.sized", .width=u_bits, .offset=u_word, .size=2 * u_word, .alignment=u_word } }) {
			SCOPED_TRACE(fc_case.function);
			const LLVMValueRef fn_read = LLVMGetNamedFunction(lm_module.get(), fc_case.function);
			ASSERT_NE(fn_read, nullptr);
			const LLVMValueRef ir_init = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_read));
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_init), LLVMInsertValue);
			const LLVMTypeRef ty_struct = LLVMTypeOf(ir_init);
			ASSERT_EQ(LLVMCountStructElementTypes(ty_struct), 2u);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_struct, 0)), 8u);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_struct, 1)), fc_case.width);
			EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_struct, 0), 0u);
			EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_struct, 1), fc_case.offset);
			EXPECT_EQ(LLVMABISizeOfType(td_target, ty_struct), fc_case.size);
			EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_struct), fc_case.alignment);
		}
	}
}

TEST (BackendLowering, StructRangeFieldsUseInlineStorageOnBothTargets) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(typedef<struct> Window {
    pub bool marked, pub range<int<12>> span, pub range<usize>? bounds, pub bool tail
}
fn<int> main() { Window window = {true, -3..=2, null, false}; return if (window.marked) 42 else 1 })",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef     mod_module  = lm_module.get();
		const LLVMContextRef    ctx_context = LLVMGetModuleContext(mod_module);
		const LLVMTargetDataRef td_layout   = LLVMGetModuleDataLayout(mod_module);
		const LLVMValueRef      fn_main     = LLVMGetNamedFunction(mod_module, "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_struct = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_struct), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_struct), 4u);
		const LLVMTypeRef ty_span      = backend::detail::IntegerRange::type(LLVMIntTypeInContext(ctx_context, 12));
		const LLVMTypeRef ty_wordRange = backend::detail::IntegerRange::type(LLVMIntTypeInContext(ctx_context, u_bits));
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_struct, 1), ty_span);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_struct, 2), backend::detail::nullable::type(ty_wordRange));
		const unsigned u_word = u_bits / CHAR_BIT;
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_struct, 1), 2u);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_struct, 2), 8u);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_struct, 3), 8u + 4u * u_word);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_layout, ty_struct), u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_layout, ty_struct), 8u + 5u * u_word);
	}
}

TEST (BackendLowering, NullableStructLocalsUseTheCheckedPayloadAlignment) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program = checkProgram(R"(typedef<struct> Sample { pub bool ready, pub usize word }
fn<int> main() { Sample? sample = {false, 0}; return if (sample == null) 7 else sample.word -> int })",
		                                                      { .sizeBits=u_bits });
		ASSERT_EQ(cp_program.localTypes.size(), 1u);
		const cloth::ResolvedType   &rt_nullable = cp_program.localTypes.begin()->second;
		const cloth::StorageLayout   sl_nullable = cp_program.storageLayout(rt_nullable).value();
		const cloth::StructLayout   &sl_payload  = cp_program.structLayouts.at(rt_nullable.element());
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());
		const LLVMValueRef      fn_main   = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_NE(ir_slot, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_nullable = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_nullable), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_nullable), 2u);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_nullable, 0), LLVMInt8TypeInContext(LLVMGetModuleContext(lm_module.get())));
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_nullable, 0), 0u);
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_nullable, 1), sl_payload.storage.alignment);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_nullable), sl_nullable.size);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_nullable), sl_nullable.alignment);
		const LLVMTypeRef ty_payload = LLVMStructGetTypeAtIndex(ty_nullable, 1);
		ASSERT_EQ(LLVMGetTypeKind(ty_payload), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_payload), 2u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_payload, 1)), u_bits);
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_payload, 1), sl_payload.fieldOffsets.at(sl_payload.fields.at(1).symbol));
	}
}

TEST (BackendLowering, GuardedStructFieldReadsTheTargetSizedFieldOnThePresentEdge) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(typedef<struct> Sample { pub bool flag, pub usize word }
fn<usize?> read(final bool present, final usize word) {
    Sample full := {false, word}
    Sample? sample := if (present) full else null
    return sample?.word
}
fn<int> main() = (read(true, 42) ?: 7) -> int)",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_read = LLVMGetNamedFunction(lm_module.get(), "cloth.read");
		ASSERT_NE(fn_read, nullptr);
		const LLVMBasicBlockRef bb_end    = LLVMGetLastBasicBlock(fn_read);
		const LLVMValueRef      ir_return = LLVMGetBasicBlockTerminator(bb_end);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
		const LLVMValueRef ir_result = LLVMGetOperand(ir_return, 0);
		ASSERT_NE(LLVMIsAPHINode(ir_result), nullptr);
		ASSERT_EQ(LLVMCountIncoming(ir_result), 2u);
		const unsigned u_absent = LLVMIsNull(LLVMGetIncomingValue(ir_result, 0)) ? 0u : 1u;
		ASSERT_TRUE(LLVMIsNull(LLVMGetIncomingValue(ir_result, u_absent)));
		const LLVMBasicBlockRef bb_present = LLVMGetIncomingBlock(ir_result, 1 - u_absent);
		const LLVMValueRef      ir_branch  = LLVMGetBasicBlockTerminator(LLVMGetIncomingBlock(ir_result, u_absent));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		EXPECT_EQ(LLVMGetSuccessor(ir_branch, 0), bb_present);
		EXPECT_EQ(LLVMGetSuccessor(ir_branch, 1), bb_end);
		const LLVMValueRef ir_present = LLVMGetIncomingValue(ir_result, 1 - u_absent);
		ASSERT_NE(LLVMIsAInsertValueInst(ir_present), nullptr);
		const LLVMValueRef ir_word = LLVMGetOperand(ir_present, 1);
		ASSERT_NE(LLVMIsAExtractValueInst(ir_word), nullptr);
		EXPECT_EQ(LLVMGetInstructionParent(ir_word), bb_present);
		EXPECT_EQ(LLVMTypeOf(ir_word), LLVMIntTypeInContext(LLVMGetModuleContext(lm_module.get()), u_bits));
	}
}

TEST (BackendLowering, NullableFixedArrayLocalsStorePresenceBeforeTheAlignedBuffer) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = read(1)
fn<int> read(final usize index) {
    usize[2]? pair = [0, 42]
    if (pair == null) return 1
    return pair[index] -> int
})",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_read = LLVMGetNamedFunction(lm_module.get(), "cloth.read");
		ASSERT_NE(fn_read, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_read));
		ASSERT_NE(LLVMIsAAllocaInst(ir_slot), nullptr);
		const LLVMTypeRef ty_local = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_local), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_local), 2u);
		const LLVMContextRef ctx_context = LLVMGetModuleContext(lm_module.get());
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_local, 0), LLVMInt8TypeInContext(ctx_context));
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_local, 1), LLVMArrayType2(LLVMIntTypeInContext(ctx_context, u_bits), 2));
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());
		const unsigned          u_word    = u_bits / 8;
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_local, 1), u_word);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_local), u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_local), 3u * u_word);
	}
}

TEST (BackendLowering, NullableFixedArraySizeIncludesItsPresenceByte) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program =
		        checkProgram("fn<int> main() { unsigned int<8>[4294967295]? bytes = null; return 42 }", { .sizeBits = u_bits });
		const backend::LoweredModule lm_module(checkProgram("fn<int> main() = 42", { .sizeBits = u_bits }));
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		ASSERT_EQ(cp_program.localTypes.size(), 1u);
		const cloth::ResolvedType &rt_nullable = cp_program.localTypes.begin()->second;
		EXPECT_NE(vt_types.localType(rt_nullable.element()), nullptr);
		EXPECT_EQ(vt_types.localType(rt_nullable) != nullptr, u_bits == 64);
	}
}

TEST (BackendLowering, FixedArrayReadsAndWritesUseTheCheckedElementStrideOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram                           cp_program = checkProgram(R"(fn<int> main() = replace(1, 42)
fn<int> replace(final usize index, final int<12> next) { int<12>[2] values = [7, 9]; values[index] = next; return values[index] -> int })",
		                                                                                { .sizeBits=u_bits });
		const auto it_array   = std::ranges::find_if(
		        cp_program.localTypes, [] (const decltype(cp_program.localTypes)::value_type &entry) { return entry.second.array(); });
		ASSERT_NE(it_array, cp_program.localTypes.end());
		EXPECT_EQ(it_array->second.arrayExtent(), 2u);
		const cloth::StorageLayout   sl_data = cp_program.arrayDataLayout(it_array->second, 2).value();
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_target  = LLVMGetModuleDataLayout(lm_module.get());
		const LLVMValueRef      fn_replace = LLVMGetNamedFunction(lm_module.get(), "cloth.replace");
		ASSERT_NE(fn_replace, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_replace));
		ASSERT_NE(ir_slot, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_array = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_array), LLVMArrayTypeKind);
		EXPECT_EQ(LLVMGetArrayLength2(ty_array), 2u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMGetElementType(ty_array)), 12u);
		EXPECT_EQ(LLVMABISizeOfType(td_target, LLVMGetElementType(ty_array)), 2u);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_array), sl_data.size);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_array), sl_data.alignment);
		ASSERT_EQ(sl_data, (cloth::StorageLayout{ 4u, 2u }));
		unsigned u_reads  = 0;
		unsigned u_writes = 0;
		for (LLVMBasicBlockRef bb_block = LLVMGetFirstBasicBlock(fn_replace); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
				if (LLVMGetInstructionOpcode(ir_inst) != LLVMGetElementPtr) continue;
				EXPECT_EQ(LLVMGetOperand(ir_inst, 0), ir_slot);
				EXPECT_EQ(LLVMGetGEPSourceElementType(ir_inst), ty_array);
				ASSERT_EQ(LLVMGetNumOperands(ir_inst), 3);
				EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(ir_inst, 2))), u_bits);
				const LLVMUseRef use_element = LLVMGetFirstUse(ir_inst);
				ASSERT_NE(use_element, nullptr);
				ASSERT_EQ(LLVMGetNextUse(use_element), nullptr);
				const LLVMValueRef ir_access = LLVMGetUser(use_element);
				if (LLVMGetInstructionOpcode(ir_access) == LLVMStore) {
					EXPECT_EQ(LLVMGetOperand(ir_access, 0), LLVMGetParam(fn_replace, 1));
					EXPECT_EQ(LLVMGetOperand(ir_access, 1), ir_inst);
					++u_writes;
				} else {
					ASSERT_EQ(LLVMGetInstructionOpcode(ir_access), LLVMLoad);
					EXPECT_EQ(LLVMTypeOf(ir_access), LLVMGetElementType(ty_array));
					++u_reads;
				}
			}
		}
		EXPECT_EQ(u_reads, 1u);
		EXPECT_EQ(u_writes, 1u);
	}
}

TEST (BackendLowering, GrowableArrayRuntimeReceivesCheckedStrideAndTargetSizedCounts) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		for (const char *s_declaration :
		     { "int<65>[] values = [7]", "bool[] values = [false]", "char[] values = ['A']", "float<16>[] values = [7.0]", "float<32>[] values = [7.0]",
		       "float<64>[] values = [7.0]", "int<65>?[] values = [null]", "bool?[] values = [false]", "char?[] values = [null]",
		       "float<16>?[] values = [null]", "float<64>?[] values = [null]", "range<int<12>>?[] values = [null]" }) {
			SCOPED_TRACE(s_declaration);
			const cloth::CheckedProgram cp_program = checkProgram(std::string("fn<int> main() { ") + s_declaration + R"(
    values.reserve(3)
    values.append(values[0])
    values.insert(1, values.removeAt(0))
    values.clear()
    return 42
})",
			                                                      { .sizeBits = u_bits });
			ASSERT_EQ(cp_program.localTypes.size(), 1u);
			const auto opt_cell = cp_program.storageLayout(cp_program.localTypes.begin()->second.element());
			if (!opt_cell) {
				ADD_FAILURE() << "Expected a checked storage layout for the array element";
				continue;
			}
			const backend::LoweredModule lm_module(cp_program);
			ASSERT_NO_THROW(lm_module.verify());
			const LLVMTypeRef                  ty_word   = LLVMIntTypeInContext(LLVMGetModuleContext(lm_module.get()), u_bits);
			const std::pair<const char *, int> v_calls[] = { { "cloth_rt_array_assign", 2 },
				                                         { "cloth_rt_array_reserve", 1 },
				                                         { "cloth_rt_array_append", -1 },
				                                         { "cloth_rt_array_insert", 1 },
				                                         { "cloth_rt_array_remove_at", 1 } };
			for (const auto &[s_name, i_wordOperand] : v_calls) {
				SCOPED_TRACE(s_name);
				const LLVMValueRef fn_runtime = LLVMGetNamedFunction(lm_module.get(), s_name);
				ASSERT_NE(fn_runtime, nullptr);
				LLVMUseRef use_call = LLVMGetFirstUse(fn_runtime);
				ASSERT_NE(use_call, nullptr);
				for (; use_call; use_call = LLVMGetNextUse(use_call)) {
					const LLVMValueRef ir_call = LLVMGetUser(use_call);
					ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
					const unsigned u_count = LLVMGetNumArgOperands(ir_call);
					ASSERT_GE(u_count, 4u);
					const LLVMValueRef ir_stride    = LLVMGetOperand(ir_call, u_count - 2);
					const LLVMValueRef ir_alignment = LLVMGetOperand(ir_call, u_count - 1);
					EXPECT_EQ(LLVMTypeOf(ir_stride), ty_word);
					EXPECT_EQ(LLVMTypeOf(ir_alignment), ty_word);
					ASSERT_NE(LLVMIsAConstantInt(ir_stride), nullptr);
					ASSERT_NE(LLVMIsAConstantInt(ir_alignment), nullptr);
					EXPECT_EQ(LLVMConstIntGetZExtValue(ir_stride), opt_cell->size);
					EXPECT_EQ(LLVMConstIntGetZExtValue(ir_alignment), opt_cell->alignment);
					if (i_wordOperand >= 0) EXPECT_EQ(LLVMTypeOf(LLVMGetOperand(ir_call, static_cast<unsigned>(i_wordOperand))), ty_word);
				}
			}
		}
	}
}

TEST (BackendLowering, RectangularArrayReadsUseInlineRowsAndCheckEachIndex) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program = checkProgram(R"(fn<int> main() = at(1, 2)
fn<int> at(final usize row, final usize col) {
    int<12>[2][3] rows := [[1, 2, 3], [4, 5, 42]]
    return rows[row][col] -> int
})",
		                                                      { .sizeBits = u_bits });
		const auto                  it_rows    = std::ranges::find_if(cp_program.localTypes, [] (const auto &local) { return local.second.array(); });
		ASSERT_NE(it_rows, cp_program.localTypes.end());
		const cloth::ResolvedType &rt_rows = it_rows->second;
		EXPECT_EQ(cp_program.arrayDataLayout(rt_rows, 1), (cloth::StorageLayout{ .size = 6, .alignment = 2 }));
		EXPECT_EQ(cp_program.arrayDataLayout(rt_rows, 2), (cloth::StorageLayout{ .size = 12, .alignment = 2 }));
		EXPECT_FALSE(cp_program.arrayDataLayout(rt_rows, cp_program.layout.maximumSize() / 6 + 1));

		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_at = LLVMGetNamedFunction(lm_module.get(), "cloth.at");
		ASSERT_NE(fn_at, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_at));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_rows = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_rows), LLVMArrayTypeKind);
		EXPECT_EQ(LLVMGetArrayLength2(ty_rows), 2u);
		const LLVMTypeRef ty_row = LLVMGetElementType(ty_rows);
		ASSERT_EQ(LLVMGetTypeKind(ty_row), LLVMArrayTypeKind);
		EXPECT_EQ(LLVMGetArrayLength2(ty_row), 3u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMGetElementType(ty_row)), 12u);
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_rows), 12u);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_rows), 2u);

		unsigned u_loads = 0;
		for (LLVMBasicBlockRef bb_block = LLVMGetFirstBasicBlock(fn_at); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
				if (LLVMGetInstructionOpcode(ir_inst) != LLVMLoad) continue;
				EXPECT_EQ(LLVMTypeOf(ir_inst), LLVMGetElementType(ty_row));
				++u_loads;
			}
		}
		EXPECT_EQ(u_loads, 1u);
	}
}

TEST (BackendLowering, RectangularArrayStoresAddressTheCellInTheMutableLocal) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = put(1, 2, 7)
fn<int> put(final usize row, final usize col, final int<12> value) {
    int<12>[2][3] rows = [[1, 2, 3], [4, 5, 42]]
    rows[row][col] = value
    return rows[1][2] -> int
})",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_put = LLVMGetNamedFunction(lm_module.get(), "cloth.put");
		ASSERT_NE(fn_put, nullptr);
		const LLVMUseRef use_value = LLVMGetFirstUse(LLVMGetParam(fn_put, 2));
		ASSERT_NE(use_value, nullptr);
		ASSERT_EQ(LLVMGetNextUse(use_value), nullptr);
		const LLVMValueRef ir_store = LLVMGetUser(use_value);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_store), LLVMStore);
		const LLVMValueRef ir_cell = LLVMGetOperand(ir_store, 1);
		ASSERT_NE(LLVMIsAGetElementPtrInst(ir_cell), nullptr);
		EXPECT_EQ(LLVMGetOperand(ir_cell, 2), LLVMGetParam(fn_put, 1));
		const LLVMValueRef ir_row = LLVMGetOperand(ir_cell, 0);
		ASSERT_NE(LLVMIsAGetElementPtrInst(ir_row), nullptr);
		EXPECT_EQ(LLVMGetOperand(ir_row, 2), LLVMGetParam(fn_put, 0));
		EXPECT_EQ(LLVMGetOperand(ir_row, 0), LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_put)));
		EXPECT_EQ(LLVMGetElementType(LLVMGetGEPSourceElementType(ir_row)), LLVMGetGEPSourceElementType(ir_cell));
	}
}

TEST (BackendLowering, RectangularArrayIterationCopiesEachRowIntoAnEntrySlot) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() {
    int<65>[2][3] rows := [[1, 2, 20], [3, 4, 22]]
    for (def row <- rows) row[2] = 0
    return 42
})",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_main);
		const LLVMTypeRef       ty_row   = LLVMArrayType2(LLVMIntTypeInContext(LLVMGetModuleContext(lm_module.get()), 65), 3);
		unsigned                u_copies = 0;
		for (LLVMBasicBlockRef bb_block = LLVMGetNextBasicBlock(bb_entry); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
				if (!LLVMIsAStoreInst(ir_inst)) continue;
				const LLVMValueRef ir_row = LLVMGetOperand(ir_inst, 0);
				if (LLVMTypeOf(ir_row) != ty_row) continue;
				++u_copies;
				const LLVMValueRef ir_binding = LLVMGetOperand(ir_inst, 1);
				ASSERT_NE(LLVMIsAAllocaInst(ir_binding), nullptr);
				EXPECT_EQ(LLVMGetInstructionParent(ir_binding), bb_entry);
				EXPECT_EQ(LLVMGetAllocatedType(ir_binding), ty_row);
				ASSERT_NE(LLVMIsALoadInst(ir_row), nullptr);
				const LLVMValueRef ir_address = LLVMGetOperand(ir_row, 0);
				ASSERT_NE(LLVMIsAGetElementPtrInst(ir_address), nullptr);
				EXPECT_EQ(LLVMGetGEPSourceElementType(ir_address), LLVMArrayType2(ty_row, 2));
				ASSERT_EQ(LLVMGetNumOperands(ir_address), 3);
				EXPECT_NE(LLVMGetOperand(ir_address, 0), ir_binding);
				const LLVMValueRef ir_index = LLVMGetOperand(ir_address, 2);
				ASSERT_NE(LLVMIsAPHINode(ir_index), nullptr);
				EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_index)), u_bits);
			}
		}
		EXPECT_EQ(u_copies, 1u);
	}
}

TEST (BackendLowering, CompoundArrayUpdateLoadsAndStoresThroughTheSameAddress) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = change(1)
fn<int> change(final usize index) { int<65>[2] values = [7, 40]; values[index] += delta(); return 42 }
fn<int<65>> delta() = 2)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_delta = LLVMGetNamedFunction(lm_module.get(), "cloth.delta");
		ASSERT_NE(fn_delta, nullptr);
		const LLVMUseRef use_delta = LLVMGetFirstUse(fn_delta);
		ASSERT_NE(use_delta, nullptr);
		ASSERT_EQ(LLVMGetNextUse(use_delta), nullptr);
		const LLVMValueRef ir_rhs = LLVMGetUser(use_delta);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_rhs), LLVMCall);
		const LLVMValueRef ir_read = LLVMGetPreviousInstruction(ir_rhs);
		ASSERT_NE(ir_read, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_read), LLVMLoad);
		const LLVMValueRef ir_address = LLVMGetOperand(ir_read, 0);
		ASSERT_NE(LLVMIsAGetElementPtrInst(ir_address), nullptr);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(ir_address, 2))), u_bits);
		const LLVMValueRef ir_sum = LLVMGetNextInstruction(ir_rhs);
		ASSERT_NE(ir_sum, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_sum), LLVMAdd);
		EXPECT_EQ(LLVMGetOperand(ir_sum, 0), ir_read);
		EXPECT_EQ(LLVMGetOperand(ir_sum, 1), ir_rhs);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_sum)), 65u);
		EXPECT_FALSE(LLVMGetNUW(ir_sum));
		EXPECT_FALSE(LLVMGetNSW(ir_sum));
		const LLVMValueRef ir_write = LLVMGetNextInstruction(ir_sum);
		ASSERT_NE(ir_write, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_write), LLVMStore);
		EXPECT_EQ(LLVMGetOperand(ir_write, 0), ir_sum);
		EXPECT_EQ(LLVMGetOperand(ir_write, 1), ir_address);
	}
}

TEST (BackendLowering, FixedIntegerArrayEqualityLoopsOverFullWidthElements) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = if (same(7)) 42 else 1
fn<bool> same(final int<65> n) { int<65>[5] a = [n, 1, 2, 3, 4]; int<65>[5] b = [7, 1, 2, 3, 4]; return a == b })",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_same = LLVMGetNamedFunction(lm_module.get(), "cloth.same");
		ASSERT_NE(fn_same, nullptr);
		unsigned u_slots       = 0;
		unsigned u_comparisons = 0;
		for (LLVMBasicBlockRef bb_block = LLVMGetFirstBasicBlock(fn_same); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
				if (LLVMIsAAllocaInst(ir_inst)) ++u_slots;
				if (!LLVMIsAICmpInst(ir_inst) || !LLVMIsALoadInst(LLVMGetOperand(ir_inst, 0))) continue;
				++u_comparisons;
				EXPECT_EQ(LLVMGetICmpPredicate(ir_inst), LLVMIntEQ);
				LLVMValueRef ir_offset = nullptr;
				for (const unsigned u_side : { 0u, 1u }) {
					const LLVMValueRef ir_load = LLVMGetOperand(ir_inst, u_side);
					ASSERT_NE(LLVMIsALoadInst(ir_load), nullptr);
					ASSERT_EQ(LLVMGetTypeKind(LLVMTypeOf(ir_load)), LLVMIntegerTypeKind);
					EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_load)), 65u);
					const LLVMValueRef ir_address = LLVMGetOperand(ir_load, 0);
					ASSERT_NE(LLVMIsAGetElementPtrInst(ir_address), nullptr);
					ASSERT_EQ(LLVMGetNumOperands(ir_address), 3);
					const LLVMValueRef ir_index = LLVMGetOperand(ir_address, 2);
					ASSERT_NE(LLVMIsAPHINode(ir_index), nullptr);
					EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_index)), u_bits);
					if (ir_offset) EXPECT_EQ(ir_index, ir_offset);
					ir_offset = ir_index;
				}
			}
		}
		EXPECT_EQ(u_slots, 2u);
		EXPECT_EQ(u_comparisons, 1u);
	}
}

TEST (BackendLowering, GrowableArrayTraversalGuardsFullWidthCellLoadsOnBothTargets) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() {
    int<65>[] values = [7, 42]
    int<65>[] saved := values
    if (values != saved) return 1
    for (def cell <- values) { values.clear(); if (cell == 42) return 42 }
    return 2
})",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		unsigned u_traversals = 0;
		for (LLVMBasicBlockRef bb_block = LLVMGetFirstBasicBlock(fn_main); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			const std::string_view sv_name = LLVMGetBasicBlockName(bb_block);
			if (sv_name != "array.equal.element" && sv_name != "for.body") continue;
			SCOPED_TRACE(sv_name);
			++u_traversals;
			const LLVMValueRef ir_index = LLVMGetFirstInstruction(bb_block);
			ASSERT_NE(LLVMIsAPHINode(ir_index), nullptr);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_index)), u_bits);
			const LLVMValueRef ir_entry = LLVMGetBasicBlockTerminator(LLVMGetIncomingBlock(ir_index, 0));
			ASSERT_TRUE(LLVMIsConditional(ir_entry));
			EXPECT_EQ(LLVMGetSuccessor(ir_entry, 0), bb_block);
			EXPECT_NE(LLVMGetSuccessor(ir_entry, 1), bb_block);
			unsigned u_reads = 0;
			for (LLVMValueRef ir = LLVMGetFirstInstruction(bb_block); ir; ir = LLVMGetNextInstruction(ir)) {
				if (!LLVMIsALoadInst(ir)) continue;
				const LLVMValueRef ir_address = LLVMGetOperand(ir, 0);
				if (!LLVMIsAGetElementPtrInst(ir_address)) continue;
				EXPECT_EQ(LLVMGetOperand(ir_address, LLVMGetNumOperands(ir_address) - 1), ir_index);
				EXPECT_EQ(LLVMTypeOf(ir), LLVMIntTypeInContext(LLVMGetModuleContext(lm_module.get()), 65));
				++u_reads;
			}
			EXPECT_EQ(u_reads, sv_name == "for.body" ? 1u : 2u);
		}
		EXPECT_EQ(u_traversals, 2u);
	}
}

TEST (BackendLowering, NullableFixedArrayEqualityGuardsCellLoadsWithBothPresenceFlags) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = if (same(true, false)) 1 else 42
fn<bool> same(final bool a, final bool b) {
    int<65>[2]? left := if (a) [0, 1] else null
    int<65>[2]? right := if (b) [0, 1] else null
    return left == right
})",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_same = LLVMGetNamedFunction(lm_module.get(), "cloth.same");
		ASSERT_NE(fn_same, nullptr);
		const LLVMBasicBlockRef bb_end    = LLVMGetLastBasicBlock(fn_same);
		const LLVMValueRef      ir_result = LLVMGetFirstInstruction(bb_end);
		ASSERT_NE(LLVMIsAPHINode(ir_result), nullptr);
		ASSERT_EQ(LLVMCountIncoming(ir_result), 2u);
		const LLVMBasicBlockRef bb_start  = LLVMGetIncomingBlock(ir_result, 0);
		const LLVMBasicBlockRef bb_cells  = LLVMGetIncomingBlock(ir_result, 1);
		const LLVMValueRef      ir_branch = LLVMGetBasicBlockTerminator(bb_start);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		EXPECT_EQ(LLVMGetSuccessor(ir_branch, 0), bb_cells);
		EXPECT_EQ(LLVMGetSuccessor(ir_branch, 1), bb_end);
		const LLVMValueRef ir_both = LLVMGetCondition(ir_branch);
		const LLVMValueRef ir_same = LLVMGetIncomingValue(ir_result, 0);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_both), LLVMAnd);
		ASSERT_NE(LLVMIsAICmpInst(ir_same), nullptr);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_same), LLVMIntEQ);
		const LLVMValueRef ir_equal = LLVMGetIncomingValue(ir_result, 1);
		ASSERT_NE(LLVMIsAICmpInst(ir_equal), nullptr);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_equal), LLVMIntEQ);
		for (const unsigned u_side : { 0u, 1u }) {
			EXPECT_EQ(LLVMGetOperand(ir_both, u_side), LLVMGetOperand(ir_same, u_side));
			const LLVMValueRef ir_cell = LLVMGetOperand(ir_equal, u_side);
			ASSERT_NE(LLVMIsALoadInst(ir_cell), nullptr);
			EXPECT_EQ(LLVMGetInstructionParent(ir_cell), bb_cells);
			EXPECT_EQ(LLVMTypeOf(ir_cell), LLVMIntTypeInContext(LLVMGetModuleContext(lm_module.get()), 65));
		}
	}
}

TEST (BackendLowering, RectangularArrayEqualityFoldsDifferentShapesAndEmptyBuffers) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = if (!differentShapes(7) && emptyRows()) 42 else 1
fn<bool> differentShapes(final int n) {
    int[1][2][1] a := [[[n], [0]]]
    int[1][1][2] b := [[[n, 0]]]
    return a == b
}
fn<bool> emptyRows() { int[2][0] a := [[], []]; return a == a })",
		                                                    { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		for (const auto &[s_name, b_expected] : { std::pair{ "cloth.differentShapes", false }, std::pair{ "cloth.emptyRows", true } }) {
			SCOPED_TRACE(s_name);
			const LLVMValueRef fn_compare = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_compare, nullptr);
			ASSERT_EQ(LLVMCountBasicBlocks(fn_compare), 1u);
			const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_compare));
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
			const LLVMValueRef ir_result = LLVMGetOperand(ir_return, 0);
			ASSERT_NE(LLVMIsAConstantInt(ir_result), nullptr);
			EXPECT_EQ(LLVMConstIntGetZExtValue(ir_result), b_expected);
		}
	}
}

TEST (BackendLowering, FixedFloatArrayInequalityUsesOrderedElementEquality) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		for (const unsigned u_width : { 16u, 32u, 64u }) {
			SCOPED_TRACE(u_width);
			const std::string            s_type = "float<" + std::to_string(u_width) + ">";
			const backend::LoweredModule lm_module(checkProgram("fn<int> main() = if (different(0.0)) 1 else 42\nfn<bool> different(final " +
			                                                            s_type + " value) { " + s_type + "[1] a := [value]; return a != a }",
			                                                    { .sizeBits=u_bits }));
			ASSERT_NO_THROW(lm_module.verify());
			const LLVMValueRef fn_different = LLVMGetNamedFunction(lm_module.get(), "cloth.different");
			ASSERT_NE(fn_different, nullptr);
			unsigned u_comparisons = 0;
			for (LLVMBasicBlockRef bb_block = LLVMGetFirstBasicBlock(fn_different); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
				for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
					if (!LLVMIsAFCmpInst(ir_inst)) continue;
					++u_comparisons;
					EXPECT_EQ(LLVMGetFCmpPredicate(ir_inst), LLVMRealOEQ);
					for (const unsigned u_side : { 0u, 1u }) {
						const LLVMValueRef ir_operand = LLVMGetOperand(ir_inst, u_side);
						EXPECT_EQ(LLVMGetTypeKind(LLVMTypeOf(ir_operand)), u_width == 64 ? LLVMDoubleTypeKind : LLVMFloatTypeKind);
						if (u_width == 16) {
							ASSERT_NE(LLVMIsACallInst(ir_operand), nullptr);
							EXPECT_EQ(LLVMGetCalledValue(ir_operand), LLVMGetNamedFunction(lm_module.get(), "cloth.fp16.extend"));
						}
					}
				}
			}
			EXPECT_EQ(u_comparisons, 1u);
		}
	}
}

TEST (BackendLowering, AbsentScalarEqualityIgnoresTheUnusedPayload) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = checkProgram("fn<int> main() { usize? value := null; return 42 }", { .sizeBits=u_bits });
		const backend::LoweredModule lm_module(cp_program);
		const LLVMContextRef         ctx_context = LLVMGetModuleContext(lm_module.get());
		const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_builder(LLVMCreateBuilderInContext(ctx_context), LLVMDisposeBuilder);
		const backend::detail::ScalarLowering                                   sl_scalars(lm_module.get(), ir_builder.get(), cp_program.layout);
		ASSERT_EQ(cp_program.localTypes.size(), 1u);
		const cloth::ResolvedType &rt_type    = cp_program.localTypes.begin()->second;
		const LLVMTypeRef          ty_payload = sl_scalars.type(rt_type.element());
		const LLVMValueRef         ir_absent  = LLVMConstNull(backend::detail::nullable::type(ty_payload));
		// Cloth source initializes an absent payload to zero. Give it poison to check that equality masks it.
		const LLVMValueRef ir_absentPoison =
		        backend::detail::nullable::pack(ir_builder.get(), LLVMGetPoison(ty_payload), LLVMConstNull(LLVMInt8TypeInContext(ctx_context)));
		const LLVMValueRef ir_present = backend::detail::nullable::presentConstant(LLVMConstNull(ty_payload));
		const LLVMTypeRef  ty_bool    = LLVMInt1TypeInContext(ctx_context);
		EXPECT_EQ(sl_scalars.compare(cloth::OEqual, ir_absentPoison, ir_absent, rt_type), LLVMConstAllOnes(ty_bool));
		EXPECT_EQ(sl_scalars.compare(cloth::ONotEqual, ir_absent, ir_absentPoison, rt_type), LLVMConstNull(ty_bool));
		EXPECT_EQ(sl_scalars.compare(cloth::OEqual, ir_present, ir_absentPoison, rt_type), LLVMConstNull(ty_bool));
		EXPECT_EQ(sl_scalars.compare(cloth::ONotEqual, ir_absentPoison, ir_present, rt_type), LLVMConstAllOnes(ty_bool));
	}
}

TEST (BackendLowering, AbsentRangeElementsIgnoreTheUnusedEndpointsAndInclusionFlag) {
	const std::unique_ptr<LLVMOpaqueContext, decltype(&LLVMContextDispose)> ctx_context(LLVMContextCreate(), LLVMContextDispose);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_builder(LLVMCreateBuilderInContext(ctx_context.get()), LLVMDisposeBuilder);
	const LLVMTypeRef  ty_range  = backend::detail::IntegerRange::type(LLVMIntTypeInContext(ctx_context.get(), 65));
	const LLVMTypeRef  ty_bool   = LLVMInt1TypeInContext(ctx_context.get());
	const LLVMValueRef ir_absent = LLVMConstNull(backend::detail::nullable::type(ty_range));
	const LLVMValueRef ir_absentPoison =
	        backend::detail::nullable::pack(ir_builder.get(), LLVMGetPoison(ty_range), LLVMConstNull(LLVMInt8TypeInContext(ctx_context.get())));
	const LLVMValueRef ir_presentEmpty = backend::detail::nullable::presentConstant(LLVMConstNull(ty_range));
	EXPECT_EQ(backend::detail::IntegerRange::equalPacked(ir_builder.get(), ir_absentPoison, ir_absent, true), LLVMConstAllOnes(ty_bool));
	EXPECT_EQ(backend::detail::IntegerRange::equalPacked(ir_builder.get(), ir_absentPoison, ir_presentEmpty, true), LLVMConstNull(ty_bool));
	EXPECT_EQ(backend::detail::IntegerRange::equalPacked(ir_builder.get(), ir_presentEmpty, ir_absentPoison, true), LLVMConstNull(ty_bool));
}

TEST (BackendLowering, FixedArrayLayoutMatchesEverySupportedIntegerWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program =
		        checkProgram("fn<int> main() { int[3] signedValues := [0, 0, 0]; unsigned int[3] unsignedValues := [0, 0, 0]; return 42 }", { .sizeBits=u_bits });
		const backend::LoweredModule lm_module(cp_program);
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		const LLVMTargetDataRef      td_target = LLVMGetModuleDataLayout(lm_module.get());
		ASSERT_EQ(cp_program.localTypes.size(), 2u);
		for (const decltype(cp_program.localTypes)::value_type &entry : cp_program.localTypes) {
			cloth::ResolvedType rt_array = entry.second;
			SCOPED_TRACE(rt_array.spelling());
			for (unsigned u_width = 1; u_width <= cloth::MAX_INTEGER_BITS; ++u_width) {
				rt_array.width             = u_width;
				const LLVMTypeRef ty_array = vt_types.localType(rt_array);
				ASSERT_NE(ty_array, nullptr) << u_width;
				ASSERT_EQ(LLVMGetArrayLength2(ty_array), 3u) << u_width;
				const LLVMTypeRef          ty_element = LLVMGetElementType(ty_array);
				const cloth::StorageLayout sl_element = cp_program.layout.integerStorage(u_width);
				ASSERT_EQ(LLVMGetIntTypeWidth(ty_element), u_width) << u_width;
				ASSERT_EQ(LLVMABISizeOfType(td_target, ty_element), sl_element.size) << u_width;
				ASSERT_EQ(LLVMABISizeOfType(td_target, ty_array), 3 * sl_element.size) << u_width;
				ASSERT_EQ(LLVMABIAlignmentOfType(td_target, ty_array), sl_element.alignment) << u_width;
			}
		}
	}
}

TEST (BackendLowering, EmbedsNestedStructFieldsAtTheirTargetAlignment) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(typedef<struct> Inner<T> { pub bool ready, pub T word, pub int<12> value }
typedef<struct> Sample { pub bool first, pub Inner<usize> inner, pub bool last }
fn<int> main() { Sample sample = {false, {true, 1, 42}, false}; sample.inner.value = 7; return sample.inner.value -> int })",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_layout = LLVMGetModuleDataLayout(lm_module.get());
		const LLVMValueRef      fn_main   = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_sample = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_sample), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_sample), 3u);
		const LLVMTypeRef ty_inner = LLVMStructGetTypeAtIndex(ty_sample, 1);
		ASSERT_EQ(LLVMGetTypeKind(ty_inner), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_inner), 3u);
		const unsigned u_word = u_bits / 8;
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_inner, 1)), u_bits);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_inner, 2)), 12u);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_inner, 1), u_word);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_inner, 2), 2 * u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_layout, ty_inner), 3 * u_word);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_layout, ty_inner), u_word);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_sample, 1), u_word);
		EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_sample, 2), 4 * u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_layout, ty_sample), 5 * u_word);
		EXPECT_EQ(LLVMABIAlignmentOfType(td_layout, ty_sample), u_word);
	}
}

TEST (BackendLowering, StoresIntoTheStructFieldSelectedByCheckedFactsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		cloth::CheckedProgram      cp_program = checkProgram(R"(typedef<struct> Pair { pub int<12> left, pub int<12> right }
fn<int> main() { Pair pair = {42, 0}; pair.left = 7; return pair.right -> int })",
		                                                     { .sizeBits=u_bits });
		const cloth::StructLayout &sl_pair    = cp_program.structLayouts.begin()->second;
		ASSERT_EQ(sl_pair.fields.size(), 2u);
		// Redirect only the write. Its source still spells the first field.
		for (cloth::ExpressionFacts::value_type &ef_value : cp_program.expressions) {
			if (ef_value.second.symbol == sl_pair.fields[0].symbol) ef_value.second.symbol = sl_pair.fields[1].symbol;
		}
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_layout = LLVMGetModuleDataLayout(lm_module.get());
		const LLVMBasicBlockRef bb_entry  = LLVMGetEntryBasicBlock(LLVMGetNamedFunction(lm_module.get(), "main"));
		const LLVMValueRef      ir_slot   = LLVMGetFirstInstruction(bb_entry);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		const LLVMTypeRef ty_pair = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_pair), LLVMStructTypeKind);
		unsigned u_fieldStores = 0;
		for (LLVMValueRef ir_inst = LLVMGetNextInstruction(ir_slot); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
			if (LLVMGetInstructionOpcode(ir_inst) != LLVMStore) continue;
			const LLVMValueRef ir_address = LLVMGetOperand(ir_inst, 1);
			if (ir_address == ir_slot) continue;
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_address), LLVMGetElementPtr);
			EXPECT_EQ(LLVMGetGEPSourceElementType(ir_address), ty_pair);
			EXPECT_EQ(LLVMGetOperand(ir_address, 0), ir_slot);
			ASSERT_EQ(LLVMGetNumOperands(ir_address), 3);
			EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetOperand(ir_address, 1)), 0u);
			EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetOperand(ir_address, 2)), 1u);
			EXPECT_EQ(LLVMOffsetOfElement(td_layout, ty_pair, 1), sl_pair.fieldOffsets.at(sl_pair.fields[1].symbol));
			const LLVMValueRef ir_value = LLVMGetOperand(ir_inst, 0);
			ASSERT_NE(LLVMIsAConstantInt(ir_value), nullptr);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_value)), 12u);
			EXPECT_EQ(LLVMConstIntGetZExtValue(ir_value), 7u);
			EXPECT_EQ(LLVMGetAlignment(ir_inst), LLVMABIAlignmentOfType(td_layout, LLVMTypeOf(ir_value)));
			++u_fieldStores;
		}
		EXPECT_EQ(u_fieldStores, 1u);
	}
}

TEST (BackendLowering, ReadsTheLocalSelectedByCheckedFacts) {
	cloth::CheckedProgram cp_program = checkProgram("fn<int> main() { int first := 42; int second := 7; return first }");
	const cloth::Token   *t_second   = nullptr;
	for (const decltype(cp_program.localTypes)::value_type &lt_local : cp_program.localTypes) {
		if (lt_local.first->getLexeme() == "second") t_second = lt_local.first;
	}
	ASSERT_NE(t_second, nullptr);
	for (cloth::ExpressionFacts::value_type &ef_value : cp_program.expressions) {
		if (ef_value.second.binding) ef_value.second.binding = t_second;
	}
	expectReturn(cp_program, 7);
}

TEST (BackendLowering, WritesAndReadsOneIntegerSlotOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram("pub fn<int> main() { int answer = 42; answer = 7; return answer }", { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_layout = LLVMGetModuleDataLayout(lm_module.get());
		EXPECT_EQ(LLVMPointerSize(td_layout) * CHAR_BIT, u_bits);
		const LLVMTypeRef  ty_integer  = LLVMInt32TypeInContext(LLVMGetModuleContext(lm_module.get()));
		const unsigned     u_alignment = LLVMABIAlignmentOfType(td_layout, ty_integer);
		const LLVMValueRef fn_main     = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMBasicBlockRef   bb_entry = LLVMGetEntryBasicBlock(fn_main);
		std::vector<LLVMValueRef> v_instructions;
		for (LLVMValueRef ir_instruction = LLVMGetFirstInstruction(bb_entry); ir_instruction; ir_instruction = LLVMGetNextInstruction(ir_instruction)) {
			v_instructions.push_back(ir_instruction);
		}
		ASSERT_EQ(v_instructions.size(), 5u);
		const LLVMValueRef ir_slot = v_instructions[0];
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_slot), LLVMAlloca);
		EXPECT_EQ(LLVMGetAllocatedType(ir_slot), ty_integer);
		EXPECT_EQ(LLVMGetAlignment(ir_slot), LLVMPreferredAlignmentOfType(td_layout, ty_integer));
		for (const std::size_t st_index : { 1u, 2u }) {
			const LLVMValueRef ir_store = v_instructions[st_index];
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_store), LLVMStore);
			EXPECT_EQ(LLVMGetAlignment(ir_store), u_alignment);
			EXPECT_EQ(LLVMGetOperand(ir_store, 1), ir_slot);
			const LLVMValueRef ir_value = LLVMGetOperand(ir_store, 0);
			ASSERT_NE(LLVMIsAConstantInt(ir_value), nullptr);
			EXPECT_EQ(LLVMConstIntGetZExtValue(ir_value), st_index == 1 ? 42u : 7u);
		}
		const LLVMValueRef ir_load = v_instructions[3];
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_load), LLVMLoad);
		EXPECT_EQ(LLVMGetAlignment(ir_load), u_alignment);
		EXPECT_EQ(LLVMGetOperand(ir_load, 0), ir_slot);
		const LLVMValueRef ir_ret = v_instructions[4];
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_ret), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_ret, 0), ir_load);
	}
}

TEST (BackendLowering, WritesTheLocalSelectedByCheckedFacts) {
	cloth::CheckedProgram cp_program = checkProgram("fn<int> main() { int first = 42; def second = 0; first = 7; return second }");
	const cloth::Token   *t_second   = nullptr;
	for (const decltype(cp_program.localTypes)::value_type &lt_local : cp_program.localTypes) {
		if (lt_local.first->getLexeme() == "second") t_second = lt_local.first;
	}
	ASSERT_NE(t_second, nullptr);
	for (decltype(cp_program.references)::value_type &ref_use : cp_program.references) {
		if (ref_use.first->getLexeme() == "first") ref_use.second = t_second;
	}
	const backend::LoweredModule lm_module(cp_program);
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
	ASSERT_NE(fn_main, nullptr);
	const LLVMValueRef ir_ret = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main));
	ASSERT_NE(ir_ret, nullptr);
	const LLVMValueRef ir_read = LLVMGetOperand(ir_ret, 0);
	ASSERT_NE(LLVMIsALoadInst(ir_read), nullptr);
	const LLVMValueRef ir_write = LLVMGetPreviousInstruction(ir_read);
	ASSERT_NE(ir_write, nullptr);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_write), LLVMStore);
	EXPECT_EQ(LLVMGetOperand(ir_write, 1), LLVMGetOperand(ir_read, 0));
}

TEST (BackendLowering, CallsALaterPrivateFunctionOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() { return identity(42) }
fn<int> identity(bool value) { println(1); return 7 }
fn<int> identity(int value) = value
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef fn_identity = LLVMGetNextFunction(fn_main);
		ASSERT_NE(fn_identity, nullptr);
		EXPECT_EQ(LLVMGetNextFunction(fn_identity), nullptr);
		EXPECT_EQ(LLVMGetLinkage(fn_identity), LLVMInternalLinkage);
		ASSERT_EQ(LLVMCountParams(fn_identity), 1u);
		const LLVMTypeRef ty_integer = LLVMInt32TypeInContext(LLVMGetModuleContext(lm_module.get()));
		EXPECT_EQ(LLVMTypeOf(LLVMGetParam(fn_identity, 0)), ty_integer);
		EXPECT_EQ(LLVMGetReturnType(LLVMGlobalGetValueType(fn_identity)), ty_integer);
		const LLVMValueRef ir_ret = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_NE(ir_ret, nullptr);
		const LLVMValueRef ir_call = LLVMGetOperand(ir_ret, 0);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_identity);
		ASSERT_EQ(LLVMGetNumArgOperands(ir_call), 1u);
		const LLVMValueRef ir_argument = LLVMGetOperand(ir_call, 0);
		ASSERT_NE(LLVMIsAConstantInt(ir_argument), nullptr);
		EXPECT_EQ(LLVMConstIntGetZExtValue(ir_argument), 42u);
	}
}

TEST (BackendLowering, BranchesOnceAndReturnsFromTheSelectedArmOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = choose(true)
fn<int> choose(final bool flag) { if (probe(flag)) { return 42 } else { return 7 } }
fn<bool> probe(final bool flag) = flag
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_choose = LLVMGetNamedFunction(lm_module.get(), "cloth.choose");
		ASSERT_NE(fn_choose, nullptr);
		ASSERT_EQ(LLVMCountBasicBlocks(fn_choose), 3u);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_choose);
		const LLVMValueRef      ir_call  = LLVMGetFirstInstruction(bb_entry);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), LLVMGetNamedFunction(lm_module.get(), "cloth.probe"));
		EXPECT_EQ(LLVMGetOperand(ir_call, 0), LLVMGetParam(fn_choose, 0));
		const LLVMValueRef ir_test = LLVMGetNextInstruction(ir_call);
		ASSERT_NE(ir_test, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_test), LLVMICmp);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_test), LLVMIntNE);
		EXPECT_EQ(LLVMGetOperand(ir_test, 0), ir_call);
		EXPECT_TRUE(LLVMIsNull(LLVMGetOperand(ir_test, 1)));
		const LLVMValueRef ir_branch = LLVMGetBasicBlockTerminator(bb_entry);
		ASSERT_EQ(LLVMGetNextInstruction(ir_test), ir_branch);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		EXPECT_EQ(LLVMGetCondition(ir_branch), ir_test);

		const unsigned u_results[] = { 42, 7 };
		for (unsigned u_arm = 0; u_arm < 2; ++u_arm) {
			const LLVMBasicBlockRef bb_arm    = LLVMGetSuccessor(ir_branch, u_arm);
			const LLVMValueRef      ir_return = LLVMGetBasicBlockTerminator(bb_arm);
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
			ASSERT_EQ(LLVMGetNumOperands(ir_return), 1);
			const LLVMValueRef ir_value = LLVMGetOperand(ir_return, 0);
			ASSERT_NE(LLVMIsAConstantInt(ir_value), nullptr);
			EXPECT_EQ(LLVMConstIntGetZExtValue(ir_value), u_results[u_arm]);
		}
	}
}

TEST (BackendLowering, JoinsBothConditionalValuesAtTheReturnOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = choose(true)
fn<int> choose(final bool flag) { return if (flag) 42 else 7 }
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_choose = LLVMGetNamedFunction(lm_module.get(), "cloth.choose");
		ASSERT_NE(fn_choose, nullptr);
		const LLVMValueRef ir_branch = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_choose));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		const LLVMBasicBlockRef bb_then = LLVMGetSuccessor(ir_branch, 0);
		const LLVMBasicBlockRef bb_else = LLVMGetSuccessor(ir_branch, 1);
		const LLVMValueRef      ir_then = LLVMGetBasicBlockTerminator(bb_then);
		const LLVMValueRef      ir_else = LLVMGetBasicBlockTerminator(bb_else);
		ASSERT_EQ(LLVMGetNumSuccessors(ir_then), 1u);
		ASSERT_EQ(LLVMGetNumSuccessors(ir_else), 1u);
		const LLVMBasicBlockRef bb_join = LLVMGetSuccessor(ir_then, 0);
		EXPECT_EQ(LLVMGetSuccessor(ir_else, 0), bb_join);
		const LLVMValueRef ir_phi = LLVMGetFirstInstruction(bb_join);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_phi), LLVMPHI);
		ASSERT_EQ(LLVMCountIncoming(ir_phi), 2u);
		for (unsigned i = 0; i < LLVMCountIncoming(ir_phi); ++i) {
			const LLVMBasicBlockRef bb_source = LLVMGetIncomingBlock(ir_phi, i);
			ASSERT_TRUE(bb_source == bb_then || bb_source == bb_else);
			const LLVMValueRef ir_value = LLVMGetIncomingValue(ir_phi, i);
			ASSERT_NE(LLVMIsAConstantInt(ir_value), nullptr);
			EXPECT_EQ(LLVMConstIntGetZExtValue(ir_value), bb_source == bb_then ? 42u : 7u);
		}
		const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(bb_join);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_return, 0), ir_phi);
	}
}

TEST (BackendLowering, JoinsOnlyTheContinuingPredecessorAfterAGuardedDivision) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = offset(false, 84, 2) -> int
fn<int<12>> offset(final bool flag, final int<12> n, final int<12> divisor) {
    int<12> selected := if (flag) { return n } else n / divisor
    return selected + 1
}
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_offset = LLVMGetNamedFunction(lm_module.get(), "cloth.offset");
		ASSERT_NE(fn_offset, nullptr);
		const LLVMValueRef ir_branch = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_offset));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		const LLVMValueRef ir_early = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 0));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_early), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_early, 0), LLVMGetParam(fn_offset, 1));
		const LLVMValueRef ir_guard = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 1));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_guard), 2u);
		const LLVMValueRef ir_overflow = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_guard, 1));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_overflow), 2u);
		const LLVMBasicBlockRef bb_valid = LLVMGetSuccessor(ir_overflow, 1);
		const LLVMValueRef      ir_exit  = LLVMGetBasicBlockTerminator(bb_valid);
		ASSERT_EQ(LLVMGetNumSuccessors(ir_exit), 1u);
		const LLVMValueRef ir_phi = LLVMGetFirstInstruction(LLVMGetSuccessor(ir_exit, 0));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_phi), LLVMPHI);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_phi)), 12u);
		ASSERT_EQ(LLVMCountIncoming(ir_phi), 1u);
		EXPECT_EQ(LLVMGetIncomingBlock(ir_phi, 0), bb_valid);
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetIncomingValue(ir_phi, 0)), LLVMSDiv);
	}
}

TEST (BackendLowering, SkipsTheCallWhenItsConditionalArgumentAlwaysReturns) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = unused(if (true) { return 42 } else 7)
fn<int> unused(int n) = n
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth.unused"), nullptr);
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef ir_branch = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 0))), LLVMRet);
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 1))), LLVMUnreachable);
	}
}

TEST (BackendLowering, EmitsOnlyTheOperandsReachedBeforeAReturn) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = (first() / (if (true) { return 42 } else 0)) + skipped()
fn<int> first() = 7
fn<int> skipped() { println(1); return 0 }
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_NE(LLVMGetNamedFunction(lm_module.get(), "cloth.first"), nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth.skipped"), nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth_rt_divide_by_zero"), nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth_rt_signed_division_overflow"), nullptr);
	}
}

TEST (BackendLowering, RechecksTheWhileConditionAndReusesLoopLocalStorage) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = sum(3)
fn<int> sum(int remaining) {
    int total = 0
    while (more(remaining)) {
        int term = remaining
        total += term
        remaining--
    }
    return total
}
fn<bool> more(final int n) = n > 0
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_sum = LLVMGetNamedFunction(lm_module.get(), "cloth.sum");
		ASSERT_NE(fn_sum, nullptr);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_sum);
		const LLVMValueRef      ir_start = LLVMGetBasicBlockTerminator(bb_entry);
		ASSERT_EQ(LLVMGetNumSuccessors(ir_start), 1u);
		const LLVMBasicBlockRef bb_test   = LLVMGetSuccessor(ir_start, 0);
		const LLVMValueRef      ir_branch = LLVMGetBasicBlockTerminator(bb_test);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		const LLVMValueRef ir_predicate = LLVMGetCondition(ir_branch);
		ASSERT_NE(LLVMIsAICmpInst(ir_predicate), nullptr);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_predicate), LLVMIntNE);
		const LLVMValueRef ir_call = LLVMGetOperand(ir_predicate, 0);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), LLVMGetNamedFunction(lm_module.get(), "cloth.more"));
		EXPECT_EQ(LLVMGetInstructionParent(ir_call), bb_test);
		const LLVMValueRef ir_back = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 0));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_back), 1u);
		EXPECT_EQ(LLVMGetSuccessor(ir_back, 0), bb_test);
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 1))), LLVMRet);

		unsigned u_slots = 0;
		for (LLVMBasicBlockRef bb = bb_entry; bb; bb = LLVMGetNextBasicBlock(bb)) {
			for (LLVMValueRef ir = LLVMGetFirstInstruction(bb); ir; ir = LLVMGetNextInstruction(ir)) {
				if (LLVMIsAAllocaInst(ir)) {
					EXPECT_EQ(bb, bb_entry);
					++u_slots;
				}
			}
		}
		EXPECT_EQ(u_slots, 3u);
	}
}

TEST (BackendLowering, TraversesReadOnlyFixedArraysWithATargetSizedIndex) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() {
    for (int unused <- []) skipped()
    int[2] digits := [4, 2]
    int result = 0
    for (def digit <- digits) result = result * 10 + digit
    return result
}
fn<void> skipped() {})",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth.skipped"), nullptr);
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_main);
		unsigned                u_arrays = 0, u_indexedReads = 0;
		for (LLVMBasicBlockRef bb_block = bb_entry; bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
				if (LLVMIsAAllocaInst(ir_inst)) {
					EXPECT_EQ(bb_block, bb_entry);
					if (LLVMGetTypeKind(LLVMGetAllocatedType(ir_inst)) == LLVMArrayTypeKind) ++u_arrays;
				}
				if (!LLVMIsAGetElementPtrInst(ir_inst)) continue;
				++u_indexedReads;
				ASSERT_EQ(LLVMGetNumOperands(ir_inst), 3);
				const LLVMValueRef ir_index = LLVMGetOperand(ir_inst, 2);
				ASSERT_NE(LLVMIsAPHINode(ir_index), nullptr);
				EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_index)), u_bits);
				EXPECT_EQ(LLVMCountIncoming(ir_index), 2u);
			}
		}
		EXPECT_EQ(u_arrays, 1u);
		EXPECT_EQ(u_indexedReads, 1u);
	}
}

TEST (BackendLowering, IntegerRangesCompareAndAdvanceAtTheCheckedWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(
		        checkProgram(R"(fn<int> main() = signedRange(-1, 1) + unsignedRange(0, 1) + signedInclusive(-1, 1) + unsignedInclusive(0, 1)
fn<int> signedRange(final int<65> first, final int<65> last) { for (def n <- first..last) n = last; return 0 }
fn<int> unsignedRange(final usize first, final usize last) { for (def n <- first..last) n = last; return 0 }
fn<int> signedInclusive(final int<65> first, final int<65> last) { for (def n <- first..=last) n = last; return 0 }
fn<int> unsignedInclusive(final usize first, final usize last) { for (def n <- first..=last) n = last; return 0 })",
		                     { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		for (const auto &[s_name, predicate] :
		     { std::pair{ "cloth.signedRange", LLVMIntSLT }, std::pair{ "cloth.unsignedRange", LLVMIntULT },
		       std::pair{ "cloth.signedInclusive", LLVMIntSLE }, std::pair{ "cloth.unsignedInclusive", LLVMIntULE } }) {
			SCOPED_TRACE(s_name);
			const unsigned     u_width  = predicate == LLVMIntSLT || predicate == LLVMIntSLE ? 65u : u_bits;
			const LLVMValueRef fn_range = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_range, nullptr);
			const LLVMBasicBlockRef bb_entry  = LLVMGetEntryBasicBlock(fn_range);
			const LLVMValueRef      ir_branch = LLVMGetBasicBlockTerminator(bb_entry);
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
			ASSERT_TRUE(LLVMIsConditional(ir_branch));
			const LLVMValueRef ir_test = LLVMGetCondition(ir_branch);
			ASSERT_NE(LLVMIsAICmpInst(ir_test), nullptr);
			EXPECT_EQ(LLVMGetICmpPredicate(ir_test), predicate);
			EXPECT_EQ(LLVMGetOperand(ir_test, 0), LLVMGetParam(fn_range, 0));
			EXPECT_EQ(LLVMGetOperand(ir_test, 1), LLVMGetParam(fn_range, 1));
			unsigned u_increments = 0;
			for (LLVMBasicBlockRef bb = bb_entry; bb; bb = LLVMGetNextBasicBlock(bb)) {
				for (LLVMValueRef ir = LLVMGetFirstInstruction(bb); ir; ir = LLVMGetNextInstruction(ir)) {
					if (LLVMGetInstructionOpcode(ir) != LLVMAdd) continue;
					++u_increments;
					EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir)), u_width);
					EXPECT_FALSE(LLVMGetNUW(ir));
					EXPECT_FALSE(LLVMGetNSW(ir));
				}
			}
			EXPECT_EQ(u_increments, 1u);
		}
	}
}

TEST (BackendLowering, InlineIntegerMembershipComparesAtTheCheckedWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = if (signedMember(0, -1, 1) && unsignedMember(1, 0, 1)) 42 else 0
fn<bool> signedMember(final int<65> n, final int<65> low, final int<65> high) = n : low..high
fn<bool> unsignedMember(final usize n, final usize low, final usize high) = low..=high == n)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		for (const bool b_signed : { true, false }) {
			const LLVMValueRef fn_member = LLVMGetNamedFunction(lm_module.get(), b_signed ? "cloth.signedMember" : "cloth.unsignedMember");
			ASSERT_NE(fn_member, nullptr);
			std::vector<LLVMIntPredicate> v_predicates;
			for (LLVMValueRef ir = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_member)); ir; ir = LLVMGetNextInstruction(ir)) {
				const LLVMOpcode op = LLVMGetInstructionOpcode(ir);
				EXPECT_NE(op, LLVMInsertValue);
				EXPECT_NE(op, LLVMExtractValue);
				if (op != LLVMICmp) continue;
				v_predicates.push_back(LLVMGetICmpPredicate(ir));
				EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(ir, 0))), b_signed ? 65u : u_bits);
			}
			EXPECT_EQ(v_predicates, (std::vector<LLVMIntPredicate>{ b_signed ? LLVMIntSLE : LLVMIntULE, b_signed ? LLVMIntSLT : LLVMIntULE }));
		}
	}
}

TEST (BackendLowering, RangeEqualityComparesBothEndpointsAtTheirFullWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = if (wide(0, 1, 0, 1) && word(0, 1, 1, 0)) 42 else 1
fn<bool> wide(final int<65> start, final int<65> end, final int<65> otherStart, final int<65> otherEnd) = start..=end == otherStart..=otherEnd
fn<bool> word(final usize start, final usize end, final usize otherStart, final usize otherEnd) = start..end != otherStart..otherEnd)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		for (const auto &[s_name, u_width] : { std::pair{ "cloth.wide", 65u }, std::pair{ "cloth.word", u_bits } }) {
			const LLVMValueRef fn_compare = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_compare, nullptr);
			unsigned u_comparisons = 0;
			for (LLVMValueRef ir = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_compare)); ir; ir = LLVMGetNextInstruction(ir)) {
				if (LLVMGetInstructionOpcode(ir) != LLVMICmp) continue;
				EXPECT_EQ(LLVMGetICmpPredicate(ir), LLVMIntEQ);
				EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(ir, 0))), u_width);
				++u_comparisons;
			}
			EXPECT_EQ(u_comparisons, 2u);
		}
	}
}

TEST (BackendLowering, StoredIntegerRangesUseTheCheckedEndpointAndFlagLayout) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = checkProgram(R"(fn<int> main() {
    def small := int<1>::min..int<1>::max
    def narrow := int<12>::min..=int<12>::max
    def wide := int<65>::min..=int<65>::max
    def largest := int<65536>::min..int<65536>::max
    def word := usize::min..=usize::max
    return 42
})",
		                                                       { .sizeBits=u_bits });
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		backend::detail::ValueTypes vt_types(lm_module.get(), cp_program);
		const LLVMTargetDataRef     td_target = LLVMGetModuleDataLayout(lm_module.get());
		ASSERT_EQ(cp_program.localTypes.size(), 5u);
		for (const auto &[name, range] : cp_program.localTypes) {
			SCOPED_TRACE(range.spelling());
			const cloth::ResolvedType *rt_endpoint = cloth::semantic::rangeEndpoint(range);
			ASSERT_NE(rt_endpoint, nullptr);
			const LLVMTypeRef ty_endpoint = vt_types.get(*rt_endpoint);
			const LLVMTypeRef ty_range    = vt_types.localType(range);
			ASSERT_NE(ty_range, nullptr);
			ASSERT_EQ(LLVMCountStructElementTypes(ty_range), 3u);
			EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_range, 0), ty_endpoint);
			EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_range, 1), ty_endpoint);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_range, 2)), 8u);
			const auto sl_endpoint = cp_program.storageLayout(*rt_endpoint).value();
			const auto sl_range    = cp_program.storageLayout(range).value();
			EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_range, 1), sl_endpoint.size);
			EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_range, 2), 2 * sl_endpoint.size);
			EXPECT_EQ(LLVMABISizeOfType(td_target, ty_range), sl_range.size);
			EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_range), sl_range.alignment);
		}
	}
}

TEST (BackendLowering, NullableIntegerRangeLocalsAlignTheRangeAfterThePresenceByte) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = checkProgram(R"(fn<int> main() {
    range<int<1>>? bit = null
    range<unsigned int<12>>? small = 0..0
    range<int<65>>? wide = -1..=0
    range<unsigned int<65536>>? largest = null
    range<usize>? word = 0..0
    return 42
})",
		                                                       { .sizeBits=u_bits });
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		backend::detail::ValueTypes vt_types(lm_module.get(), cp_program);
		const LLVMTargetDataRef     td_target = LLVMGetModuleDataLayout(lm_module.get());
		ASSERT_EQ(cp_program.localTypes.size(), 5u);
		for (const auto &[name, range] : cp_program.localTypes) {
			SCOPED_TRACE(range.spelling());
			const LLVMTypeRef ty_nullable = vt_types.localType(range);
			ASSERT_NE(ty_nullable, nullptr);
			ASSERT_EQ(LLVMGetTypeKind(ty_nullable), LLVMStructTypeKind);
			ASSERT_EQ(LLVMCountStructElementTypes(ty_nullable), 2u);
			EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_nullable, 0), LLVMInt8TypeInContext(LLVMGetModuleContext(lm_module.get())));
			EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_nullable, 1), vt_types.localType(range.element()));
			const cloth::StorageLayout sl_range    = cp_program.storageLayout(range.element()).value();
			const cloth::StorageLayout sl_nullable = cp_program.storageLayout(range).value();
			EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_nullable, 0), 0u);
			EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_nullable, 1), sl_range.alignment);
			EXPECT_EQ(LLVMABISizeOfType(td_target, ty_nullable), sl_nullable.size);
			EXPECT_EQ(LLVMABIAlignmentOfType(td_target, ty_nullable), sl_nullable.alignment);
		}
	}
}

TEST (BackendLowering, PostfixRangeStepsUseTheCounterWidthAndAllowOverflowDetection) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() {
    for (int<65> n <- int<65>::min..=int<65>::max; n++) n = int<65>::max
    for (usize n <- usize::min..=usize::max; n++) n = usize::max
    for (int<65> n <- int<65>::max..=int<65>::min; n--) n = int<65>::min
    for (usize n <- usize::max..=usize::min; n--) n = usize::min
    return 0
})",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		for (const LLVMOpcode op_step : { LLVMAdd, LLVMSub }) {
			SCOPED_TRACE(op_step);
			std::vector<unsigned> v_widths;
			for (LLVMBasicBlockRef bb = LLVMGetEntryBasicBlock(fn_main); bb; bb = LLVMGetNextBasicBlock(bb)) {
				for (LLVMValueRef ir = LLVMGetFirstInstruction(bb); ir; ir = LLVMGetNextInstruction(ir)) {
					if (LLVMGetInstructionOpcode(ir) != op_step) continue;
					v_widths.push_back(LLVMGetIntTypeWidth(LLVMTypeOf(ir)));
					EXPECT_FALSE(LLVMGetNSW(ir));
					EXPECT_FALSE(LLVMGetNUW(ir));
				}
			}
			std::ranges::sort(v_widths);
			EXPECT_EQ(v_widths, (std::vector<unsigned>{ u_bits, 65u }));
		}
	}
}

TEST (BackendLowering, RangeAdditionAndSubtractionCheckOverflowAtTheCounterWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = walk(-2, 2)
fn<int> walk(final int<65> signedStep, final usize unsignedStep) {
    for (int<65> n <- int<65>::max..=int<65>::min; n += signedStep) n = int<65>::min
    for (usize n <- 0..=usize::max; n += unsignedStep) n = usize::max
    for (int<65> n <- int<65>::min..=int<65>::max; n -= signedStep) n = int<65>::max
    for (usize n <- usize::max..=0; n -= unsignedStep) n = 0
    return 0
})",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const std::string s_unsignedAdd = "llvm.uadd.with.overflow.i" + std::to_string(u_bits);
		const std::string s_unsignedSub = "llvm.usub.with.overflow.i" + std::to_string(u_bits);
		for (const char *s_name : { "llvm.sadd.with.overflow.i65", s_unsignedAdd.c_str(), "llvm.ssub.with.overflow.i65", s_unsignedSub.c_str() }) {
			SCOPED_TRACE(s_name);
			const LLVMValueRef fn_update = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_update, nullptr);
			ASSERT_NE(LLVMGetFirstUse(fn_update), nullptr);
			EXPECT_NE(LLVMIsACallInst(LLVMGetUser(LLVMGetFirstUse(fn_update))), nullptr);
		}
	}
}

TEST (BackendLowering, RangeProductsUseCheckedNativeOrMultiwordMultiplication) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = walk(-2, 2, -1)
fn<int> walk(final isize signedFactor, final usize unsignedFactor, final int<129> wideFactor) {
    for (isize n <- isize::min..=isize::max; n *= signedFactor) n = isize::max
    for (usize n <- 0..=usize::max; n *= unsignedFactor) n = usize::max
    for (int<129> n <- int<129>::min..=int<129>::max; n *= wideFactor) n = int<129>::min
    return 0
})",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const std::string s_signed   = "llvm.smul.with.overflow.i" + std::to_string(u_bits);
		const std::string s_unsigned = "llvm.umul.with.overflow.i" + std::to_string(u_bits);
		for (const char *s_name : { s_signed.c_str(), s_unsigned.c_str(), "cloth_rt_mul_words" }) {
			const LLVMValueRef fn_mul = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_mul, nullptr) << s_name;
			ASSERT_NE(LLVMGetFirstUse(fn_mul), nullptr) << s_name;
		}
		const LLVMValueRef fn_walk = LLVMGetNamedFunction(lm_module.get(), "cloth.walk");
		ASSERT_NE(fn_walk, nullptr);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_walk);
		for (LLVMBasicBlockRef bb = LLVMGetNextBasicBlock(bb_entry); bb; bb = LLVMGetNextBasicBlock(bb))
			for (LLVMValueRef ir = LLVMGetFirstInstruction(bb); ir; ir = LLVMGetNextInstruction(ir))
				EXPECT_NE(LLVMGetInstructionOpcode(ir), LLVMAlloca);
	}
}

TEST (BackendLowering, RangeDivisionAndRemainderUseTheSignednessAndWidthOfTheCounter) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = walk(-2, 2, 2, 2)
fn<int> walk(final isize signedNative, final usize unsignedNative, final int<97> signedWide, final unsigned int<65> unsignedWide) {
    for (isize n <- 24..=6; n /= signedNative) {}
    for (usize n <- 24..=6; n /= unsignedNative) {}
    for (int<97> n <- 24..=6; n /= signedWide) {}
    for (unsigned int<65> n <- 24..=6; n /= unsignedWide) {}
    for (isize n <- 35..=7; n %= signedNative) {}
    for (usize n <- 35..=7; n %= unsignedNative) {}
    for (int<97> n <- 35..=7; n %= signedWide) {}
    for (unsigned int<65> n <- 35..=7; n %= unsignedWide) {}
    return 0
})",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_walk   = LLVMGetNamedFunction(lm_module.get(), "cloth.walk");
		const LLVMValueRef fn_divmod = LLVMGetNamedFunction(lm_module.get(), "cloth_rt_divmod_words");
		ASSERT_NE(fn_walk, nullptr);
		ASSERT_NE(fn_divmod, nullptr);
		const LLVMBasicBlockRef                    bb_entry = LLVMGetEntryBasicBlock(fn_walk);
		std::vector<LLVMOpcode>                    v_native;
		std::vector<std::pair<unsigned, unsigned>> v_wide;
		for (LLVMBasicBlockRef bb = bb_entry; bb; bb = LLVMGetNextBasicBlock(bb)) {
			for (LLVMValueRef ir = LLVMGetFirstInstruction(bb); ir; ir = LLVMGetNextInstruction(ir)) {
				const LLVMOpcode op = LLVMGetInstructionOpcode(ir);
				if (op == LLVMAlloca) EXPECT_EQ(bb, bb_entry);
				if (op == LLVMSDiv || op == LLVMUDiv || op == LLVMSRem || op == LLVMURem) {
					EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir)), u_bits);
					if (op == LLVMSDiv || op == LLVMUDiv) EXPECT_FALSE(LLVMGetExact(ir));
					v_native.push_back(op);
				}
				if (op == LLVMCall && LLVMGetCalledValue(ir) == fn_divmod) {
					v_wide.emplace_back(LLVMConstIntGetZExtValue(LLVMGetOperand(ir, 3)), LLVMConstIntGetZExtValue(LLVMGetOperand(ir, 4)));
				}
			}
		}
		for (const LLVMOpcode op : { LLVMSDiv, LLVMUDiv, LLVMSRem, LLVMURem }) EXPECT_EQ(std::ranges::count(v_native, op), 1);
		std::ranges::sort(v_wide);
		EXPECT_EQ(v_wide, (std::vector<std::pair<unsigned, unsigned>>{ { 3, 0 }, { 3, 0 }, { 4, 1 }, { 4, 1 } }));
	}
}

TEST (BackendLowering, RangeAssignmentUsesTheReplacementAsTheNextCounter) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() {
    for (isize n <- 0..=19; n = replacement()) { if (n == 19) break }
    return 42
}
fn<isize> replacement() = 19)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_replacement = LLVMGetNamedFunction(lm_module.get(), "cloth.replacement");
		ASSERT_NE(fn_replacement, nullptr);
		const LLVMUseRef use_call = LLVMGetFirstUse(fn_replacement);
		ASSERT_NE(use_call, nullptr);
		EXPECT_EQ(LLVMGetNextUse(use_call), nullptr);
		const LLVMValueRef ir_call = LLVMGetUser(use_call);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_call)), u_bits);
		bool b_feedsCounter = false;
		for (LLVMUseRef use = LLVMGetFirstUse(ir_call); use; use = LLVMGetNextUse(use)) {
			if (LLVMIsAPHINode(LLVMGetUser(use))) b_feedsCounter = true;
		}
		EXPECT_TRUE(b_feedsCounter);
	}
}

TEST (BackendLowering, LeavesEndlessLoopsWithoutAnImplicitReturn) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		for (const char *s_source : { "pub fn<int> main() { while (true) {} }", "pub fn<int> main() { do {} while (true) }" }) {
			SCOPED_TRACE(s_source);
			const backend::LoweredModule lm_module(checkProgram(s_source, { .sizeBits=u_bits }));
			ASSERT_NO_THROW(lm_module.verify());
			const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
			ASSERT_NE(fn_main, nullptr);
			for (LLVMBasicBlockRef bb = LLVMGetEntryBasicBlock(fn_main); bb; bb = LLVMGetNextBasicBlock(bb)) {
				const LLVMValueRef ir_exit = LLVMGetBasicBlockTerminator(bb);
				ASSERT_EQ(LLVMGetInstructionOpcode(ir_exit), LLVMBr);
				EXPECT_FALSE(LLVMIsConditional(ir_exit));
			}
		}
	}
}

TEST (BackendLowering, ReturnsFromAWhileConditionBeforeEnteringTheBody) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = answer(true)
fn<int> answer(final bool flag) {
    while (if (flag) { return 42 } else { return 7 }) { unused() }
}
fn<void> unused() {}
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth.unused"), nullptr);
	}
}

TEST (BackendLowering, TestsTheDoWhileConditionAfterTheBody) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() {
    do { touch() } while (again())
    return 42
}
fn<void> touch() {}
fn<bool> again() = false
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMValueRef ir_start = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_start), 1u);
		const LLVMBasicBlockRef bb_body  = LLVMGetSuccessor(ir_start, 0);
		const LLVMValueRef      ir_touch = LLVMGetFirstInstruction(bb_body);
		ASSERT_NE(LLVMIsACallInst(ir_touch), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_touch), LLVMGetNamedFunction(lm_module.get(), "cloth.touch"));
		const LLVMValueRef ir_bodyExit = LLVMGetBasicBlockTerminator(bb_body);
		ASSERT_EQ(LLVMGetNumSuccessors(ir_bodyExit), 1u);
		const LLVMBasicBlockRef bb_test   = LLVMGetSuccessor(ir_bodyExit, 0);
		const LLVMValueRef      ir_branch = LLVMGetBasicBlockTerminator(bb_test);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
		ASSERT_TRUE(LLVMIsConditional(ir_branch));
		const LLVMValueRef ir_predicate = LLVMGetCondition(ir_branch);
		ASSERT_NE(LLVMIsAICmpInst(ir_predicate), nullptr);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_predicate), LLVMIntNE);
		const LLVMValueRef ir_again = LLVMGetOperand(ir_predicate, 0);
		ASSERT_NE(LLVMIsACallInst(ir_again), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_again), LLVMGetNamedFunction(lm_module.get(), "cloth.again"));
		EXPECT_EQ(LLVMGetInstructionParent(ir_again), bb_test);
		EXPECT_EQ(LLVMGetSuccessor(ir_branch, 0), bb_body);
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 1))), LLVMRet);
	}
}

TEST (BackendLowering, SkipsTheDoWhileConditionWhenTheBodyReturns) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(
		        checkProgram("pub fn<int> main() { do { return 42 } while (unused()) }\nfn<bool> unused() = true", { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth.unused"), nullptr);
	}
}

TEST (BackendLowering, UsesTheCheckedLoopTargetForANestedContinue) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		cloth::CheckedProgram cp_program = checkProgram(R"(pub fn<int> main() = route(false, false)
fn<int> route(final bool outer, final bool inner) {
    while (outer) {
        while (inner) { continue }
        break
    }
    return 42
}
)",
		                                                { .sizeBits=u_bits });
		ASSERT_EQ(cp_program.jumpTargets.size(), 2u);
		const cloth::LoopTarget *lt_outer = nullptr;
		for (const auto &[jump, target] : cp_program.jumpTargets) {
			if (!jump->isContinue()) lt_outer = target;
		}
		ASSERT_NE(lt_outer, nullptr);
		for (auto &[jump, target] : cp_program.jumpTargets) {
			if (jump->isContinue()) {
				ASSERT_NE(target, lt_outer);
				target = lt_outer;
			}
		}
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_route = LLVMGetNamedFunction(lm_module.get(), "cloth.route");
		ASSERT_NE(fn_route, nullptr);
		const LLVMValueRef ir_start = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_route));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_start), 1u);
		const LLVMBasicBlockRef bb_outerTest = LLVMGetSuccessor(ir_start, 0);
		const LLVMValueRef      ir_outerTest = LLVMGetBasicBlockTerminator(bb_outerTest);
		ASSERT_EQ(LLVMGetNumSuccessors(ir_outerTest), 2u);
		const LLVMValueRef ir_innerStart = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_outerTest, 0));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_innerStart), 1u);
		const LLVMValueRef ir_innerTest = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_innerStart, 0));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_innerTest), 2u);
		const LLVMValueRef ir_continue = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_innerTest, 0));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_continue), 1u);
		EXPECT_EQ(LLVMGetSuccessor(ir_continue, 0), bb_outerTest);
	}
}

TEST (BackendLowering, SkipsTheDoWhileConditionAfterBreakWithADeadContinue) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() {
    do { if (false) continue; break } while (unused())
    return 42
}
fn<bool> unused() = true
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth.unused"), nullptr);
	}
}

TEST (BackendLowering, KeepsADeadBreakFromOpeningAnEndlessLoop) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		for (const char *s_source :
		     { "fn<int> main() { while (true) { if (false) break } }", "fn<int> main() { do { if (false) break; continue } while (true) }" }) {
			SCOPED_TRACE(s_source);
			const backend::LoweredModule lm_module(checkProgram(s_source, { .sizeBits=u_bits }));
			ASSERT_NO_THROW(lm_module.verify());
			const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
			ASSERT_NE(fn_main, nullptr);
			for (LLVMBasicBlockRef bb = LLVMGetEntryBasicBlock(fn_main); bb; bb = LLVMGetNextBasicBlock(bb)) {
				EXPECT_NE(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(bb)), LLVMRet);
			}
		}
	}
}

TEST (BackendLowering, CallsAPrivateVoidHelperBeforeReturningOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram("pub fn<int> main() { touch(); return 42 }\nfn<void> touch() {}", { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMBasicBlockRef bb_main = LLVMGetEntryBasicBlock(fn_main);
		const LLVMValueRef      ir_call = LLVMGetFirstInstruction(bb_main);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetNumArgOperands(ir_call), 0u);
		const LLVMValueRef ir_ret = LLVMGetBasicBlockTerminator(bb_main);
		ASSERT_NE(ir_ret, nullptr);
		EXPECT_EQ(LLVMGetNextInstruction(ir_call), ir_ret);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_ret), LLVMRet);
		ASSERT_EQ(LLVMGetNumOperands(ir_ret), 1);
		const LLVMValueRef ir_answer = LLVMGetOperand(ir_ret, 0);
		ASSERT_NE(LLVMIsAConstantInt(ir_answer), nullptr);
		EXPECT_EQ(LLVMConstIntGetZExtValue(ir_answer), 42u);

		const LLVMValueRef fn_touch = LLVMGetCalledValue(ir_call);
		EXPECT_EQ(LLVMGetLinkage(fn_touch), LLVMInternalLinkage);
		const LLVMTypeRef ty_void = LLVMVoidTypeInContext(LLVMGetModuleContext(lm_module.get()));
		EXPECT_EQ(LLVMTypeOf(ir_call), ty_void);
		EXPECT_EQ(LLVMGetReturnType(LLVMGlobalGetValueType(fn_touch)), ty_void);
		ASSERT_EQ(LLVMCountBasicBlocks(fn_touch), 1u);
		const LLVMBasicBlockRef bb_helper  = LLVMGetEntryBasicBlock(fn_touch);
		const LLVMValueRef      ir_voidRet = LLVMGetBasicBlockTerminator(bb_helper);
		ASSERT_NE(ir_voidRet, nullptr);
		EXPECT_EQ(LLVMGetFirstInstruction(bb_helper), ir_voidRet);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_voidRet), LLVMRet);
		EXPECT_EQ(LLVMGetNumOperands(ir_voidRet), 0);
	}
}

TEST (BackendLowering, CallsOneExternalInt32PrintlnDeclarationOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() { println(1); echo(2); return 42 }
fn<void> echo(final int value) = println(value)
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef mod_module = lm_module.get();
		const LLVMValueRef  fn_println = LLVMGetNamedFunction(mod_module, "cloth_rt_println_i32");
		ASSERT_NE(fn_println, nullptr);
		EXPECT_TRUE(LLVMIsDeclaration(fn_println));
		EXPECT_EQ(LLVMGetLinkage(fn_println), LLVMExternalLinkage);
		EXPECT_EQ(LLVMGetFunctionCallConv(fn_println), LLVMCCallConv);
		const LLVMTypeRef ty_function = LLVMGlobalGetValueType(fn_println);
		EXPECT_FALSE(LLVMIsFunctionVarArg(ty_function));
		ASSERT_EQ(LLVMCountParamTypes(ty_function), 1u);
		EXPECT_EQ(LLVMTypeOf(LLVMGetParam(fn_println, 0)), LLVMInt32TypeInContext(LLVMGetModuleContext(mod_module)));
		EXPECT_EQ(LLVMGetReturnType(ty_function), LLVMVoidTypeInContext(LLVMGetModuleContext(mod_module)));
		for (const char *s_caller : { "main", "cloth.echo" }) {
			const LLVMValueRef fn_caller = LLVMGetNamedFunction(mod_module, s_caller);
			ASSERT_NE(fn_caller, nullptr);
			const LLVMValueRef ir_call = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_caller));
			ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
			EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_println);
			EXPECT_EQ(LLVMGetInstructionCallConv(ir_call), LLVMCCallConv);
		}
	}
}

TEST (BackendLowering, OmitsThePrintRuntimeForShadowedAndUnreachedCalls) {
	for (const char *s_source : {
	             "fn<int> main() = println(1)\nfn<int> println(final int value) = value + 41",
	             "fn<int> main() { println(if (true) { return 42 } else 1) }",
	     }) {
		SCOPED_TRACE(s_source);
		const backend::LoweredModule lm_module(checkProgram(s_source));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth_rt_println_i32"), nullptr);
	}
}

TEST (BackendLowering, PassesTheCheckedInt32StatusToANonReturningExitOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = stop(7)
fn<int> stop(final unsigned int<97> status) = exit(status)
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef mod_module = lm_module.get();
		const LLVMValueRef  fn_exit    = LLVMGetNamedFunction(mod_module, "cloth_rt_exit");
		ASSERT_NE(fn_exit, nullptr);
		EXPECT_TRUE(LLVMIsDeclaration(fn_exit));
		EXPECT_EQ(LLVMGetLinkage(fn_exit), LLVMExternalLinkage);
		EXPECT_EQ(LLVMGetFunctionCallConv(fn_exit), LLVMCCallConv);
		const LLVMTypeRef ty_function = LLVMGlobalGetValueType(fn_exit);
		ASSERT_EQ(LLVMCountParamTypes(ty_function), 1u);
		EXPECT_FALSE(LLVMIsFunctionVarArg(ty_function));
		const LLVMTypeRef ty_status = LLVMInt32TypeInContext(LLVMGetModuleContext(mod_module));
		EXPECT_EQ(LLVMTypeOf(LLVMGetParam(fn_exit, 0)), ty_status);
		EXPECT_EQ(LLVMGetTypeKind(LLVMGetReturnType(ty_function)), LLVMVoidTypeKind);
		constexpr std::string_view sv_noReturn = "noreturn";
		const unsigned             u_attribute = LLVMGetEnumAttributeKindForName(sv_noReturn.data(), sv_noReturn.size());
		EXPECT_NE(LLVMGetEnumAttributeAtIndex(fn_exit, LLVMAttributeFunctionIndex, u_attribute), nullptr);

		const LLVMValueRef fn_stop = LLVMGetNamedFunction(mod_module, "cloth.stop");
		ASSERT_NE(fn_stop, nullptr);
		const LLVMBasicBlockRef bb_entry  = LLVMGetEntryBasicBlock(fn_stop);
		const LLVMValueRef      ir_status = LLVMGetFirstInstruction(bb_entry);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_status), LLVMTrunc);
		EXPECT_EQ(LLVMGetOperand(ir_status, 0), LLVMGetParam(fn_stop, 0));
		EXPECT_EQ(LLVMTypeOf(ir_status), ty_status);
		const LLVMValueRef ir_call = LLVMGetNextInstruction(ir_status);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_exit);
		EXPECT_EQ(LLVMGetOperand(ir_call, 0), ir_status);
		const LLVMValueRef ir_end = LLVMGetBasicBlockTerminator(bb_entry);
		ASSERT_EQ(LLVMGetNextInstruction(ir_call), ir_end);
		EXPECT_EQ(LLVMGetInstructionOpcode(ir_end), LLVMUnreachable);
	}
}

TEST (BackendLowering, PassesTheUtf8ByteSpanToANonReturningExitOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = exit(7, "é\0😀"))", { .sizeBits = u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef  mod_module  = lm_module.get();
		const LLVMContextRef ctx_context = LLVMGetModuleContext(mod_module);
		const LLVMValueRef   fn_exit     = LLVMGetNamedFunction(mod_module, "cloth_rt_exit_message");
		ASSERT_NE(fn_exit, nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(mod_module, "cloth_rt_exit"), nullptr);
		EXPECT_EQ(LLVMGetFunctionCallConv(fn_exit), LLVMCCallConv);
		const LLVMTypeRef ty_function = LLVMGlobalGetValueType(fn_exit);
		ASSERT_EQ(LLVMCountParamTypes(ty_function), 3u);
		EXPECT_FALSE(LLVMIsFunctionVarArg(ty_function));
		EXPECT_EQ(LLVMGetReturnType(ty_function), LLVMVoidTypeInContext(ctx_context));
		EXPECT_EQ(LLVMTypeOf(LLVMGetParam(fn_exit, 0)), LLVMInt32TypeInContext(ctx_context));
		EXPECT_EQ(LLVMTypeOf(LLVMGetParam(fn_exit, 1)), LLVMPointerTypeInContext(ctx_context, 0));
		EXPECT_EQ(LLVMTypeOf(LLVMGetParam(fn_exit, 2)), LLVMIntTypeInContext(ctx_context, u_bits));
		constexpr std::string_view sv_noReturn = "noreturn";
		const unsigned             u_attribute = LLVMGetEnumAttributeKindForName(sv_noReturn.data(), sv_noReturn.size());
		EXPECT_NE(LLVMGetEnumAttributeAtIndex(fn_exit, LLVMAttributeFunctionIndex, u_attribute), nullptr);

		const LLVMValueRef      fn_main  = LLVMGetNamedFunction(mod_module, "main");
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_main);
		const LLVMValueRef      ir_call  = LLVMGetFirstInstruction(bb_entry);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_exit);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetOperand(ir_call, 0)), 7u);
		EXPECT_EQ(LLVMGetOperand(ir_call, 1), LLVMGetNamedGlobal(mod_module, "cloth.string.data"));
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetOperand(ir_call, 2)), 7u);
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetNextInstruction(ir_call)), LLVMUnreachable);
	}
}

TEST (BackendLowering, OmitsTheExitRuntimeWhenAnArgumentReturnsFromTheFunction) {
	for (const char *s_source : { R"(fn<int> main() = exit(if (true) { return 42 } else { return 7 }, "unreached"))",
	                              R"(fn<int> main() = exit(7, if (true) { return 42 } else { return 7 }))" }) {
		SCOPED_TRACE(s_source);
		const backend::LoweredModule lm_module(checkProgram(s_source));
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth_rt_exit"), nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth_rt_exit_message"), nullptr);
		EXPECT_EQ(LLVMGetFirstGlobal(lm_module.get()), nullptr);
	}
}

void expectWrappingParameterOperation (const cloth::CheckedProgram &program, const char *functionName, LLVMOpcode opcode) {
	const backend::LoweredModule lm_module(program);
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef fn_helper = LLVMGetNamedFunction(lm_module.get(), functionName);
	ASSERT_NE(fn_helper, nullptr);
	ASSERT_EQ(LLVMCountParams(fn_helper), 2u);
	const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_helper));
	const LLVMValueRef ir_result = LLVMGetOperand(ir_return, 0);
	ASSERT_NE(LLVMIsABinaryOperator(ir_result), nullptr);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_result), opcode);
	EXPECT_EQ(LLVMTypeOf(ir_result), LLVMInt32TypeInContext(LLVMGetModuleContext(lm_module.get())));
	EXPECT_EQ(LLVMGetOperand(ir_result, 0), LLVMGetParam(fn_helper, 0));
	EXPECT_EQ(LLVMGetOperand(ir_result, 1), LLVMGetParam(fn_helper, 1));
	EXPECT_FALSE(LLVMGetNSW(ir_result));
	EXPECT_FALSE(LLVMGetNUW(ir_result));
}

TEST (BackendLowering, AddsTwoIntegerParametersWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectWrappingParameterOperation(checkProgram("pub fn<int> main() = add(40, 2)\nfn<int> add(final int left, final int right) = left + right",
		                                              { .sizeBits=u_bits }),
		                                 "cloth.add", LLVMAdd);
		expectReturn(checkProgram("fn<int> main() = 2147483647 + 1", { .sizeBits=u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, SubtractsTwoIntegerParametersWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectWrappingParameterOperation(
		        checkProgram("pub fn<int> main() = subtract(44, 2)\nfn<int> subtract(final int left, final int right) = left - right", { .sizeBits=u_bits }),
		        "cloth.subtract", LLVMSub);
		expectReturn(checkProgram("fn<int> main() = (0 - 2147483647 - 1) - 1", { .sizeBits=u_bits }), 2147483647u);
	}
}

TEST (BackendLowering, MultipliesTwoIntegerParametersWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectWrappingParameterOperation(
		        checkProgram("pub fn<int> main() = multiply(6, 7)\nfn<int> multiply(final int left, final int right) = left * right", { .sizeBits=u_bits }),
		        "cloth.multiply", LLVMMul);
		expectReturn(checkProgram("fn<int> main() = 2147483647 * 2", { .sizeBits=u_bits }), 0xfffffffeu);
		expectReturn(checkProgram("fn<int> main() = 65536 * 65536", { .sizeBits=u_bits }), 0u);
	}
}

TEST (BackendLowering, NegatesAnIntegerOperandOnceWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = negate(-42)
fn<int> negate(final int value) = -operand(value)
fn<int> operand(final int value) = value
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_negate = LLVMGetNamedFunction(lm_module.get(), "cloth.negate");
		ASSERT_NE(fn_negate, nullptr);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_negate);
		const LLVMValueRef      ir_call  = LLVMGetFirstInstruction(bb_entry);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), LLVMGetNamedFunction(lm_module.get(), "cloth.operand"));
		EXPECT_EQ(LLVMGetOperand(ir_call, 0), LLVMGetParam(fn_negate, 0));

		const LLVMValueRef ir_negation = LLVMGetNextInstruction(ir_call);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_negation), LLVMSub);
		EXPECT_EQ(LLVMTypeOf(ir_negation), LLVMInt32TypeInContext(LLVMGetModuleContext(lm_module.get())));
		EXPECT_TRUE(LLVMIsNull(LLVMGetOperand(ir_negation, 0)));
		EXPECT_EQ(LLVMGetOperand(ir_negation, 1), ir_call);
		EXPECT_FALSE(LLVMGetNSW(ir_negation));
		EXPECT_FALSE(LLVMGetNUW(ir_negation));
		const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(bb_entry);
		ASSERT_EQ(LLVMGetNextInstruction(ir_negation), ir_return);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_return, 0), ir_negation);
	}
}

TEST (BackendLowering, NegatesIntegerLiteralsIncludingTheSignedMinimumOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() = -42", { .sizeBits=u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = -(-42)", { .sizeBits=u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = -0", { .sizeBits=u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = -2147483648", { .sizeBits=u_bits }), 0x80000000u);
		expectReturn(checkProgram("fn<int> main() = -(-2147483648)", { .sizeBits=u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, UnaryPlusReturnsItsOperandDirectlyOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(
		        checkProgram("pub fn<int> main() = +positive(42)\nfn<int> positive(final int value) = +value", { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_positive = LLVMGetNamedFunction(lm_module.get(), "cloth.positive");
		ASSERT_NE(fn_positive, nullptr);
		const LLVMBasicBlockRef bb_positive    = LLVMGetEntryBasicBlock(fn_positive);
		const LLVMValueRef      ir_valueReturn = LLVMGetBasicBlockTerminator(bb_positive);
		ASSERT_EQ(LLVMGetFirstInstruction(bb_positive), ir_valueReturn);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_valueReturn), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_valueReturn, 0), LLVMGetParam(fn_positive, 0));

		const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		ASSERT_NE(fn_main, nullptr);
		const LLVMBasicBlockRef bb_main = LLVMGetEntryBasicBlock(fn_main);
		const LLVMValueRef      ir_call = LLVMGetFirstInstruction(bb_main);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_positive);
		const LLVMValueRef ir_callReturn = LLVMGetBasicBlockTerminator(bb_main);
		ASSERT_EQ(LLVMGetNextInstruction(ir_call), ir_callReturn);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_callReturn), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_callReturn, 0), ir_call);
	}
}

TEST (BackendLowering, UnaryPlusPreservesSignedIntegerLiteralsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() = +42", { .sizeBits=u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = +(-42)", { .sizeBits=u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = +0", { .sizeBits=u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = +2147483647", { .sizeBits=u_bits }), 0x7fffffffu);
		expectReturn(checkProgram("fn<int> main() = +(-2147483648)", { .sizeBits=u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, ComplementsAnIntegerOperandOnceOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = invert(-43)
fn<int> invert(final int value) = ~operand(value)
fn<int> operand(final int value) = value
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_invert = LLVMGetNamedFunction(lm_module.get(), "cloth.invert");
		ASSERT_NE(fn_invert, nullptr);
		const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_invert);
		const LLVMValueRef      ir_call  = LLVMGetFirstInstruction(bb_entry);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), LLVMGetNamedFunction(lm_module.get(), "cloth.operand"));
		EXPECT_EQ(LLVMGetOperand(ir_call, 0), LLVMGetParam(fn_invert, 0));

		const LLVMValueRef ir_complement = LLVMGetNextInstruction(ir_call);
		ASSERT_NE(ir_complement, nullptr);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_complement), LLVMXor);
		const LLVMTypeRef ty_integer = LLVMInt32TypeInContext(LLVMGetModuleContext(lm_module.get()));
		EXPECT_EQ(LLVMTypeOf(ir_complement), ty_integer);
		EXPECT_EQ(LLVMGetOperand(ir_complement, 0), ir_call);
		EXPECT_EQ(LLVMGetOperand(ir_complement, 1), LLVMConstAllOnes(ty_integer));
		const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(bb_entry);
		ASSERT_EQ(LLVMGetNextInstruction(ir_complement), ir_return);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
		EXPECT_EQ(LLVMGetOperand(ir_return, 0), ir_complement);
	}
}

TEST (BackendLowering, ComplementsIntegerLiteralsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() = ~0", { .sizeBits=u_bits }), 0xffffffffu);
		expectReturn(checkProgram("fn<int> main() = ~(-1)", { .sizeBits=u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = ~2147483647", { .sizeBits=u_bits }), 0x80000000u);
		expectReturn(checkProgram("fn<int> main() = ~(-2147483648)", { .sizeBits=u_bits }), 0x7fffffffu);
		expectReturn(checkProgram("fn<int> main() = ~~42", { .sizeBits=u_bits }), 42u);
	}
}

void expectGuardedDivisionOperation (LLVMModuleRef module, const char *functionName, LLVMOpcode opcode) {
	SCOPED_TRACE(functionName);
	const LLVMValueRef fn_operation = LLVMGetNamedFunction(module, functionName);
	const LLVMValueRef fn_operand   = LLVMGetNamedFunction(module, "cloth.operand");
	ASSERT_NE(fn_operation, nullptr);
	ASSERT_NE(fn_operand, nullptr);
	const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_operation);
	const LLVMValueRef      ir_left  = LLVMGetFirstInstruction(bb_entry);
	ASSERT_NE(LLVMIsACallInst(ir_left), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_left), fn_operand);
	EXPECT_EQ(LLVMGetOperand(ir_left, 0), LLVMGetParam(fn_operation, 0));
	const LLVMValueRef ir_right = LLVMGetNextInstruction(ir_left);
	ASSERT_NE(LLVMIsACallInst(ir_right), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_right), fn_operand);
	EXPECT_EQ(LLVMGetOperand(ir_right, 0), LLVMGetParam(fn_operation, 1));

	const LLVMValueRef ir_branch = LLVMGetBasicBlockTerminator(bb_entry);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_branch), LLVMBr);
	ASSERT_EQ(LLVMGetNumSuccessors(ir_branch), 2u);
	const LLVMValueRef ir_zero = LLVMGetCondition(ir_branch);
	ASSERT_NE(LLVMIsAICmpInst(ir_zero), nullptr);
	EXPECT_EQ(LLVMGetICmpPredicate(ir_zero), LLVMIntEQ);
	EXPECT_EQ(LLVMGetOperand(ir_zero, 0), ir_right);
	EXPECT_TRUE(LLVMIsNull(LLVMGetOperand(ir_zero, 1)));
	const LLVMBasicBlockRef bb_zero  = LLVMGetSuccessor(ir_branch, 0);
	const LLVMValueRef      ir_error = LLVMGetFirstInstruction(bb_zero);
	ASSERT_NE(LLVMIsACallInst(ir_error), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_error), LLVMGetNamedFunction(module, "cloth_rt_divide_by_zero"));
	EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(bb_zero)), LLVMUnreachable);

	const LLVMValueRef ir_overflow = LLVMGetBasicBlockTerminator(LLVMGetSuccessor(ir_branch, 1));
	ASSERT_EQ(LLVMGetNumSuccessors(ir_overflow), 2u);
	const LLVMBasicBlockRef bb_overflow     = LLVMGetSuccessor(ir_overflow, 0);
	const LLVMValueRef      ir_overflowCall = LLVMGetFirstInstruction(bb_overflow);
	ASSERT_NE(LLVMIsACallInst(ir_overflowCall), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_overflowCall), LLVMGetNamedFunction(module, "cloth_rt_signed_division_overflow"));
	EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(bb_overflow)), LLVMUnreachable);

	const LLVMBasicBlockRef bb_valid  = LLVMGetSuccessor(ir_overflow, 1);
	const LLVMValueRef      ir_return = LLVMGetBasicBlockTerminator(bb_valid);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_return), LLVMRet);
	const LLVMValueRef ir_result = LLVMGetOperand(ir_return, 0);
	ASSERT_NE(LLVMIsABinaryOperator(ir_result), nullptr);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_result), opcode);
	EXPECT_EQ(LLVMGetInstructionParent(ir_result), bb_valid);
	EXPECT_EQ(LLVMTypeOf(ir_result), LLVMInt32TypeInContext(LLVMGetModuleContext(module)));
	EXPECT_EQ(LLVMGetOperand(ir_result, 0), ir_left);
	EXPECT_EQ(LLVMGetOperand(ir_result, 1), ir_right);
	if (opcode == LLVMSDiv) {
		EXPECT_FALSE(LLVMGetExact(ir_result));
	}
}

TEST (BackendLowering, GuardsSignedDivisionAndRemainderAfterEvaluatingBothOperandsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = remainder(divide(284, 2), 100)
fn<int> divide(final int left, final int right) = operand(left) / operand(right)
fn<int> remainder(final int left, final int right) = operand(left) % operand(right)
fn<int> operand(final int value) = value
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		expectGuardedDivisionOperation(lm_module.get(), "cloth.divide", LLVMSDiv);
		expectGuardedDivisionOperation(lm_module.get(), "cloth.remainder", LLVMSRem);
	}
}

TEST (BackendLowering, GuardsWideUnsignedDivisorsWithANonReturningRuntimeCall) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = remainder(84, 2) -> int
fn<unsigned int<97>> remainder(final unsigned int<97> n, final unsigned int<97> d) = n % d
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef mod_module = lm_module.get();
		const LLVMValueRef  fn_failure = LLVMGetNamedFunction(mod_module, "cloth_rt_divide_by_zero");
		ASSERT_NE(fn_failure, nullptr);
		EXPECT_TRUE(LLVMIsDeclaration(fn_failure));
		EXPECT_EQ(LLVMGetLinkage(fn_failure), LLVMExternalLinkage);
		EXPECT_EQ(LLVMGetFunctionCallConv(fn_failure), LLVMCCallConv);
		const LLVMTypeRef ty_failure = LLVMGlobalGetValueType(fn_failure);
		EXPECT_EQ(LLVMGetTypeKind(LLVMGetReturnType(ty_failure)), LLVMVoidTypeKind);
		EXPECT_EQ(LLVMCountParamTypes(ty_failure), 0u);
		EXPECT_FALSE(LLVMIsFunctionVarArg(ty_failure));
		constexpr std::string_view sv_noReturn = "noreturn";
		const unsigned             u_attribute = LLVMGetEnumAttributeKindForName(sv_noReturn.data(), sv_noReturn.size());
		EXPECT_NE(LLVMGetEnumAttributeAtIndex(fn_failure, LLVMAttributeFunctionIndex, u_attribute), nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(mod_module, "cloth_rt_signed_division_overflow"), nullptr);

		const LLVMValueRef fn_remainder = LLVMGetNamedFunction(mod_module, "cloth.remainder");
		ASSERT_NE(fn_remainder, nullptr);
		const LLVMValueRef ir_guard = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_remainder));
		ASSERT_EQ(LLVMGetNumSuccessors(ir_guard), 2u);
		const LLVMValueRef ir_zero = LLVMGetCondition(ir_guard);
		ASSERT_NE(LLVMIsAICmpInst(ir_zero), nullptr);
		EXPECT_EQ(LLVMGetICmpPredicate(ir_zero), LLVMIntEQ);
		EXPECT_EQ(LLVMGetOperand(ir_zero, 0), LLVMGetParam(fn_remainder, 1));
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(ir_zero, 1))), 97u);
		EXPECT_TRUE(LLVMIsNull(LLVMGetOperand(ir_zero, 1)));
		const LLVMValueRef ir_call = LLVMGetFirstInstruction(LLVMGetSuccessor(ir_guard, 0));
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_failure);
		EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetFirstInstruction(LLVMGetSuccessor(ir_guard, 1))), LLVMURem);
	}
}

TEST (BackendLowering, GuardsFloatToIntegerCastsBeforeConvertingAtTheTargetWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("pub fn<int> main() = 42.75 -> int", { .sizeBits=u_bits }), 42u);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = (asSigned(42.75) -> int) + (asUnsigned(0.5) -> int)
fn<isize> asSigned(final float<64> value) = value -> isize
fn<usize> asUnsigned(final float<64> value) = value -> usize
)",
		                                                    { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMValueRef fn_error = LLVMGetNamedFunction(lm_module.get(), "cloth_rt_invalid_float_to_integer");
		ASSERT_NE(fn_error, nullptr);
		constexpr std::string_view sv_noReturn = "noreturn";
		const unsigned             u_attribute = LLVMGetEnumAttributeKindForName(sv_noReturn.data(), sv_noReturn.size());
		EXPECT_NE(LLVMGetEnumAttributeAtIndex(fn_error, LLVMAttributeFunctionIndex, u_attribute), nullptr);
		for (const bool b_unsigned : { false, true }) {
			const char *s_name = b_unsigned ? "cloth.asUnsigned" : "cloth.asSigned";
			SCOPED_TRACE(s_name);
			const LLVMValueRef fn_cast = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_cast, nullptr);
			const LLVMValueRef ir_guard = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_cast));
			ASSERT_EQ(LLVMGetNumSuccessors(ir_guard), 2u);
			const LLVMBasicBlockRef bb_error = LLVMGetSuccessor(ir_guard, 0);
			const LLVMValueRef      ir_error = LLVMGetFirstInstruction(bb_error);
			ASSERT_NE(LLVMIsACallInst(ir_error), nullptr);
			EXPECT_EQ(LLVMGetCalledValue(ir_error), fn_error);
			EXPECT_EQ(LLVMGetInstructionOpcode(LLVMGetBasicBlockTerminator(bb_error)), LLVMUnreachable);
			const LLVMValueRef ir_cast = LLVMGetFirstInstruction(LLVMGetSuccessor(ir_guard, 1));
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_cast), b_unsigned ? LLVMFPToUI : LLVMFPToSI);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(ir_cast)), u_bits);
			EXPECT_EQ(LLVMGetOperand(ir_cast, 0), LLVMGetParam(fn_cast, 0));
		}
	}
}

TEST (BackendLowering, TruncatesSignedQuotientsTowardZeroOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() = 85 / 2", { .sizeBits=u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (0 - 85) / 2", { .sizeBits=u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = 85 / (0 - 2)", { .sizeBits=u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = (0 - 85) / (0 - 2)", { .sizeBits=u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (2147483647 + 1) / 1", { .sizeBits=u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, SignedRemaindersHaveTheSignOfTheDividendOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() = 142 % 100", { .sizeBits=u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (0 - 142) % 100", { .sizeBits=u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = 142 % (0 - 100)", { .sizeBits=u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (0 - 142) % (0 - 100)", { .sizeBits=u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = 142 % (0 - 1)", { .sizeBits=u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = (2147483647 + 1) % 3", { .sizeBits=u_bits }), 0xfffffffeu);
	}
}

void expectBinaryCallOrder (const std::string &entry, LLVMOpcode opcode) {
	SCOPED_TRACE(entry);
	const backend::LoweredModule lm_module(checkProgram(entry + "\nfn<int> first() = 44\nfn<int> second() = 2"));
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef      fn_main  = LLVMGetNamedFunction(lm_module.get(), "main");
	const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_main);
	const LLVMValueRef      ir_left  = LLVMGetFirstInstruction(bb_entry);
	ASSERT_NE(LLVMIsACallInst(ir_left), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_left), LLVMGetNamedFunction(lm_module.get(), "cloth.first"));
	const LLVMValueRef ir_right = LLVMGetNextInstruction(ir_left);
	ASSERT_NE(LLVMIsACallInst(ir_right), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_right), LLVMGetNamedFunction(lm_module.get(), "cloth.second"));
	const LLVMValueRef ir_result = LLVMGetNextInstruction(ir_right);
	ASSERT_NE(LLVMIsABinaryOperator(ir_result), nullptr);
	ASSERT_EQ(LLVMGetInstructionOpcode(ir_result), opcode);
	EXPECT_EQ(LLVMGetOperand(ir_result, 0), ir_left);
	EXPECT_EQ(LLVMGetOperand(ir_result, 1), ir_right);
	const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(bb_entry);
	EXPECT_EQ(LLVMGetNextInstruction(ir_result), ir_return);
	EXPECT_EQ(LLVMGetOperand(ir_return, 0), ir_result);
}

TEST (BackendLowering, EvaluatesArithmeticOperandsOnceInSourceOrder) {
	expectBinaryCallOrder("fn<int> main() = first() + second()", LLVMAdd);
	expectBinaryCallOrder("fn<int> main() = first() - second()", LLVMSub);
	expectBinaryCallOrder("fn<int> main() = first() * second()", LLVMMul);
}

TEST (BackendLowering, EvaluatesForwardedVoidCallsAndDiscardedIntegerCalls) {
	const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() { relay(7); return 42 }
fn<void> relay(final int n) = middle(n)
fn middle(final int n) { return finish(n) }
fn<void> finish(final int n) { identity(n); return }
fn<int> identity(final int n) = n
)"));
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
	ASSERT_NE(fn_main, nullptr);
	const LLVMValueRef ir_entryCall = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main));
	ASSERT_NE(LLVMIsACallInst(ir_entryCall), nullptr);
	LLVMValueRef fn_helper = LLVMGetCalledValue(ir_entryCall);
	for (const char *s_name : { "cloth.relay", "cloth.middle", "cloth.finish" }) {
		SCOPED_TRACE(s_name);
		ASSERT_EQ(fn_helper, LLVMGetNamedFunction(lm_module.get(), s_name));
		const LLVMBasicBlockRef bb_helper = LLVMGetEntryBasicBlock(fn_helper);
		const LLVMValueRef      ir_call   = LLVMGetFirstInstruction(bb_helper);
		ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
		ASSERT_EQ(LLVMGetNumArgOperands(ir_call), 1u);
		EXPECT_EQ(LLVMGetOperand(ir_call, 0), LLVMGetParam(fn_helper, 0));
		const LLVMValueRef ir_ret = LLVMGetBasicBlockTerminator(bb_helper);
		ASSERT_NE(ir_ret, nullptr);
		EXPECT_EQ(LLVMGetNextInstruction(ir_call), ir_ret);
		ASSERT_EQ(LLVMGetInstructionOpcode(ir_ret), LLVMRet);
		EXPECT_EQ(LLVMGetNumOperands(ir_ret), 0);
		fn_helper = LLVMGetCalledValue(ir_call);
	}
	EXPECT_EQ(fn_helper, LLVMGetNamedFunction(lm_module.get(), "cloth.identity"));
}

TEST (BackendLowering, UsesTheCalleeSelectedByTheFrontend) {
	cloth::CheckedProgram cp_program =
	        checkProgram("fn<int> main() = written(42)\nfn<int> written(int x) { println(1); return x }\nfn<int> selected(int x) = 7");
	const cloth::SymbolScope &ss_globals   = cp_program.units.front()->getGlobals();
	const cloth::Symbol      &sym_written  = ss_globals.at("written").front();
	const cloth::Symbol      &sym_selected = ss_globals.at("selected").front();
	for (cloth::ExpressionFacts::value_type &ef_expression : cp_program.expressions) {
		if (ef_expression.second.symbol == &sym_written) ef_expression.second.symbol = &sym_selected;
	}
	const backend::LoweredModule lm_module(cp_program);
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef fn_main   = LLVMGetNamedFunction(lm_module.get(), "main");
	const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main));
	const LLVMValueRef ir_call   = LLVMGetOperand(ir_return, 0);
	ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
	const LLVMValueRef fn_selected = LLVMGetCalledValue(ir_call);
	const LLVMValueRef ir_value    = LLVMGetOperand(LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_selected)), 0);
	ASSERT_NE(LLVMIsAConstantInt(ir_value), nullptr);
	EXPECT_EQ(LLVMConstIntGetZExtValue(ir_value), 7u);
}

TEST (BackendLowering, EvaluatesNamedArgumentsInSourceOrderAndPassesThemInParameterOrder) {
	const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = choose(last = identity(7), first = identity(42))
fn<int> choose(int first, int last) = first
fn<int> identity(int value) = value
)"));
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef      fn_main  = LLVMGetNamedFunction(lm_module.get(), "main");
	const LLVMBasicBlockRef bb_entry = LLVMGetEntryBasicBlock(fn_main);
	const LLVMValueRef      ir_first = LLVMGetFirstInstruction(bb_entry);
	ASSERT_NE(LLVMIsACallInst(ir_first), nullptr);
	const LLVMValueRef ir_second = LLVMGetNextInstruction(ir_first);
	ASSERT_NE(LLVMIsACallInst(ir_second), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_first), LLVMGetCalledValue(ir_second));
	const LLVMValueRef ir_seven    = LLVMGetOperand(ir_first, 0);
	const LLVMValueRef ir_fortyTwo = LLVMGetOperand(ir_second, 0);
	ASSERT_NE(LLVMIsAConstantInt(ir_seven), nullptr);
	ASSERT_NE(LLVMIsAConstantInt(ir_fortyTwo), nullptr);
	EXPECT_EQ(LLVMConstIntGetZExtValue(ir_seven), 7u);
	EXPECT_EQ(LLVMConstIntGetZExtValue(ir_fortyTwo), 42u);
	const LLVMValueRef ir_choose = LLVMGetOperand(LLVMGetBasicBlockTerminator(bb_entry), 0);
	ASSERT_NE(LLVMIsACallInst(ir_choose), nullptr);
	ASSERT_EQ(LLVMGetNumArgOperands(ir_choose), 2u);
	EXPECT_EQ(LLVMGetOperand(ir_choose, 0), ir_second);
	EXPECT_EQ(LLVMGetOperand(ir_choose, 1), ir_first);
}

TEST (BackendLowering, ReusesDeclarationsAcrossMutualRecursion) {
	const backend::LoweredModule lm_module(checkProgram("fn<int> main() = first(42)\nfn<int> first(int n) = second(n)\nfn<int> second(int n) = first(n)"));
	ASSERT_NO_THROW(lm_module.verify());
	const LLVMValueRef fn_main  = LLVMGetNamedFunction(lm_module.get(), "main");
	const LLVMValueRef fn_first = LLVMGetNextFunction(fn_main);
	ASSERT_NE(fn_first, nullptr);
	const LLVMValueRef fn_second = LLVMGetNextFunction(fn_first);
	ASSERT_NE(fn_second, nullptr);
	EXPECT_EQ(LLVMGetNextFunction(fn_second), nullptr);
	const LLVMValueRef ir_return = LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_second));
	const LLVMValueRef ir_call   = LLVMGetOperand(ir_return, 0);
	ASSERT_NE(LLVMIsACallInst(ir_call), nullptr);
	EXPECT_EQ(LLVMGetCalledValue(ir_call), fn_first);
}

void expectUnsupported (const std::string &source, Code code, std::string_view markedText, std::size_t line = 1, cloth::TargetLayout layout = {}) {
	SCOPED_TRACE(source);
	const cloth::CheckedProgram cp_program = checkProgram(source, layout);
	try {
		const backend::LoweredModule lm_module(cp_program);
		FAIL() << "Expected a lowering diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.getCode(), code) << error.what();
		ASSERT_EQ(error.getSourceLabels().size(), 1u);
		const cloth::TokenSpan &ts_span = error.getSourceLabels().front().span;
		ASSERT_NE(ts_span.source, nullptr);
		EXPECT_EQ(ts_span.source->getPath(), "entry.co");
		ASSERT_EQ(ts_span.startLine, line);
		ASSERT_EQ(ts_span.endLine, line);
		std::istringstream iss_source(source);
		std::string        s_line;
		for (std::size_t st_index = 0; st_index < line; ++st_index) ASSERT_TRUE(std::getline(iss_source, s_line));
		EXPECT_EQ(s_line.substr(ts_span.startCol - 1, ts_span.endCol - ts_span.startCol), markedText);
	}
}

TEST (BackendLowering, UsesTheCheckedSignednessAndEmitsWrappingArithmetic) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(
		        R"(pub fn<int> main() = signedLess(-1, 1).toInt() + unsignedLess(4095, 1).toInt() + (sum(4095, 2) -> int) + (product(-2, 3) -> int)
fn<bool> signedLess(final int<12> left, final int<12> right) = left < right
fn<bool> unsignedLess(final unsigned int<12> left, final unsigned int<12> right) = left < right
fn<unsigned int<12>> sum(final unsigned int<12> left, final unsigned int<12> right) = left + right
fn<int<97>> product(final int<97> left, final int<97> right) = left * right
)",
		        { .sizeBits=u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		for (const std::pair<const char *, LLVMIntPredicate> &comparison :
		     { std::pair{ "cloth.signedLess", LLVMIntSLT }, std::pair{ "cloth.unsignedLess", LLVMIntULT } }) {
			const LLVMValueRef fn_compare = LLVMGetNamedFunction(lm_module.get(), comparison.first);
			ASSERT_NE(fn_compare, nullptr);
			const LLVMValueRef ir_compare = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_compare));
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_compare), LLVMICmp);
			EXPECT_EQ(LLVMGetICmpPredicate(ir_compare), comparison.second);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(ir_compare, 0))), 12u);
		}
		for (const std::pair<const char *, LLVMOpcode> &operation : { std::pair{ "cloth.sum", LLVMAdd }, std::pair{ "cloth.product", LLVMMul } }) {
			const LLVMValueRef fn_operation = LLVMGetNamedFunction(lm_module.get(), operation.first);
			ASSERT_NE(fn_operation, nullptr);
			const LLVMValueRef ir_result = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_operation));
			ASSERT_EQ(LLVMGetInstructionOpcode(ir_result), operation.second);
			EXPECT_FALSE(LLVMGetNSW(ir_result));
			EXPECT_FALSE(LLVMGetNUW(ir_result));
		}
	}
}

TEST (BackendLowering, PreservesPrimitiveWidthsAndTruthAtTheBoundaries) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() { bool b := 0; return b.toInt() }", { .sizeBits=u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = (false == 1).toInt()", { .sizeBits=u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = (true == -2).toInt()", { .sizeBits=u_bits }), 1);
		expectReturn(checkProgram("fn<int> main() = (int<65536>::max + 1 == int<65536>::min).toInt()", { .sizeBits=u_bits }), 1);
		expectReturn(checkProgram("fn<int> main() = (unsigned int<1>::max + 1 == 0).toInt()", { .sizeBits=u_bits }), 1);
		expectReturn(checkProgram("fn<int> main() = isize::size -> int", { .sizeBits=u_bits }), u_bits / 8);
		expectReturn(checkProgram("fn<int> main() = ((-1 -> int<12>) -> int<40>) -> int", { .sizeBits=u_bits }), 0xffffffffu);
		expectReturn(checkProgram("fn<int> main() = ((-1 -> unsigned int<12>) -> int<40>) -> int", { .sizeBits=u_bits }), 4095);
	}
}

TEST (BackendLowering, LabelsUnsupportedStatementsInSourceOrder) {
	expectUnsupported("fn<int> main() { def stop := exit(7); return 42 }", Code::UnsupportedLowering, "return 42");
	expectUnsupported("pub fn<int> main() { print(1); return 42 }", Code::UnsupportedLowering, "print(1)");
	expectUnsupported("pub fn<int> main() { return 42; println(1) }", Code::UnsupportedLowering, "println(1)");
	expectUnsupported("pub fn<int> main() { return 42; return 7 }", Code::UnsupportedLowering, "return 7");
	expectUnsupported("fn<int> main() { int a := 42; print(a); return a }", Code::UnsupportedLowering, "print(a)");
	expectUnsupported("fn<int> main() { return 42; int a := 7 }", Code::UnsupportedLowering, "int a := 7");
	expectUnsupported("fn<int> main() { int a = 42; return a; a = 7 }", Code::UnsupportedLowering, "a = 7");
	expectUnsupported("fn<int> main() { touch(); return 42 }\nfn<void> touch() { return; other() }\nfn other() {}", Code::UnsupportedLowering, "other()",
	                  2);
	expectUnsupported("fn<int> main() { if (true) { return 42; println(1) } else { return 7 } }", Code::UnsupportedLowering, "println(1)");
	expectUnsupported("fn<int> main() { if (false) { return 42 } else { return 7 }; println(1) }", Code::UnsupportedLowering, "println(1)");
	expectUnsupported("fn<int> main() { while (true) {}; return 7 }", Code::UnsupportedLowering, "return 7");
	expectUnsupported("fn<int> main() { do {} while (true); return 7 }", Code::UnsupportedLowering, "return 7");
	expectUnsupported("fn<int> main() { while (true) { break; println(1) }; return 7 }", Code::UnsupportedLowering, "println(1)");
	expectUnsupported("fn<int> main() { do { continue; return 7 } while (false); return 42 }", Code::UnsupportedLowering, "return 7");
}

TEST (BackendLowering, LabelsUnsupportedConditionalArmExpressions) {
	expectUnsupported("fn<int> main() { if (true) { print(1) }; return 7 }", Code::UnsupportedLowering, "print(1)");
	expectUnsupported("fn<int> main() = if (true) 42 else (int[2]::type::rank -> int)", Code::UnsupportedLowering, "int[2]::type::rank");
}

TEST (BackendLowering, LabelsUnsupportedLocalBindingsAndInitializers) {
	expectUnsupported(R"(typedef<struct> S { pub typeinfo kind, pub int value }
fn<int> main() { S s = {int::type, 42}; s.value = 7; return s.value })",
	                  Code::UnsupportedLowering, "S", 2);
	expectUnsupported(R"(typedef<struct> S { pub typeinfo kind, pub int value }
fn<int> main() { S s := {int::type, 42}; return s.value })",
	                  Code::UnsupportedLowering, "S", 2);
	expectUnsupported(R"(typedef<struct> Leaf { pub typeinfo kind }
typedef<struct> Box { pub Leaf item }
fn<int> main() { Box value = {{int::type}}; return 7 })",
	                  Code::UnsupportedLowering, "Box", 3);
	expectUnsupported("fn<int> main() { typeinfo? kind = null; return 42 }", Code::UnsupportedLowering, "typeinfo?");
	expectUnsupported("typedef<struct> S { pub typeinfo kind }\nfn<int> main() { S? value = null; return 42 }", Code::UnsupportedLowering, "S?", 2);
	expectUnsupported("fn<int> main() { int[][] a := [[7]]; return 42 }", Code::UnsupportedLowering, "int[][]");
	expectUnsupported("fn<int> main() { int[1][] rows := [[7, 42]]; return 42 }", Code::UnsupportedLowering, "int[1][]");
	expectUnsupported("fn<int> main() { int unused := helper(int[2]::type::rank -> int); return 42 }\nfn<int> helper(int ignored) = 7",
	                  Code::UnsupportedLowering, "int[2]::type::rank");
}

TEST (BackendLowering, StringLiteralStoresItsUtf8BytesAndTargetSizedByteLength) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program =
		        checkProgram(R"(fn<int> main() { string text = "A\0é😀"; return text::size -> int })", { .sizeBits = u_bits });
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMModuleRef mod_module = lm_module.get();
		const LLVMValueRef  ir_data    = LLVMGetFirstGlobal(mod_module);
		ASSERT_NE(ir_data, nullptr);
		EXPECT_EQ(LLVMGetLinkage(ir_data), LLVMPrivateLinkage);
		EXPECT_TRUE(LLVMIsGlobalConstant(ir_data));
		const LLVMValueRef ir_bytes = LLVMGetInitializer(ir_data);
		ASSERT_TRUE(LLVMIsConstantString(ir_bytes));
		std::size_t st_size = 0;
		const char *c_bytes = LLVMGetAsString(ir_bytes, &st_size);
		EXPECT_EQ(std::string_view(c_bytes, st_size), std::string_view("A\0\xC3\xA9\xF0\x9F\x98\x80\0", 9));
		const LLVMValueRef fn_main = LLVMGetNamedFunction(mod_module, "main");
		const LLVMValueRef ir_slot = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main));
		ASSERT_NE(LLVMIsAAllocaInst(ir_slot), nullptr);
		const LLVMTypeRef ty_text = LLVMGetAllocatedType(ir_slot);
		ASSERT_EQ(LLVMGetTypeKind(ty_text), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_text), 2u);
		EXPECT_EQ(LLVMGetTypeKind(LLVMStructGetTypeAtIndex(ty_text, 0)), LLVMPointerTypeKind);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_text, 1), LLVMIntTypeInContext(LLVMGetModuleContext(mod_module), u_bits));
		const LLVMTargetDataRef    td_target = LLVMGetModuleDataLayout(mod_module);
		const cloth::StorageLayout sl_text{ .size = LLVMABISizeOfType(td_target, ty_text), .alignment = LLVMABIAlignmentOfType(td_target, ty_text) };
		EXPECT_EQ(cp_program.storageLayout(cloth::ResolvedType(cloth::KString)), sl_text);
		const LLVMValueRef ir_store = LLVMGetNextInstruction(ir_slot);
		ASSERT_NE(LLVMIsAStoreInst(ir_store), nullptr);
		const LLVMValueRef ir_text = LLVMGetOperand(ir_store, 0);
		EXPECT_EQ(LLVMGetAggregateElement(ir_text, 0), ir_data);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_text, 1)), 8u);
	}
}

TEST (BackendLowering, LabelsUnsupportedAssignments) {
	expectUnsupported("fn<int> main() { int a = 42; a += (int[2]::type::rank -> int); return a }", Code::UnsupportedLowering, "int[2]::type::rank");
	expectUnsupported("fn<int> main() { int a = 42; a = (int[2]::type::rank -> int); a = 7; return a }", Code::UnsupportedLowering, "int[2]::type::rank");
}

TEST (BackendLowering, LabelsUnsupportedExpressions) {
	expectUnsupported(R"(fn<int> main() = exit(7, if (int[2]::type::rank == 1) "finished" else ""))", Code::UnsupportedLowering, "int[2]::type::rank");
	expectUnsupported("pub fn<int> main() = 40 - (int[2]::type::rank -> int)", Code::UnsupportedLowering, "int[2]::type::rank");
	expectUnsupported("pub fn<int> main() = (int[2]::type::rank -> int) + (if (true) { return 42 } else 0)", Code::UnsupportedLowering,
	                  "int[2]::type::rank");
	expectUnsupported("pub fn<int> main() = +(int[2]::type::rank -> int)", Code::UnsupportedLowering, "int[2]::type::rank");
	expectUnsupported("pub fn<int> main() = ~(int[2]::type::rank -> int) ** 2", Code::UnsupportedLowering, "int[2]::type::rank");
	expectUnsupported("pub fn<int> main() = sideEffect()\nfn<int> sideEffect() { print(1)\nreturn 42 }", Code::UnsupportedLowering, "print(1)", 2);
}

TEST (BackendLowering, LabelsUnsupportedEnumPayloadFieldTypes) {
	const std::string source = "typedef<enum> Mode(pub def<int[]> row = [1, 2]) { pub On }\n";
	expectUnsupported(source + "fn<int> main() { Mode mode = Mode.On; mode.row; return 42 }", Code::UnsupportedLowering, "mode.row", 2);
}

TEST (BackendLowering, LabelsUnsupportedPrintlnForms) {
	for (const std::string s_call : { "println()", "println(true)", "println(1 -> int<64>)", "println(1 -> unsigned int)", "println(\"1\")" }) {
		expectUnsupported("fn<int> main() { " + s_call + "; return 42 }", Code::UnsupportedLowering, s_call);
	}
}

TEST (BackendLowering, EmitsDirectOperatorsAndWidthSelectedOperands) {
	const std::string s_declarations = R"(
typedef<class> Item(int seed) {
    pub Item() : (0) {}
    pub operator +() -> int = 7
    pub operator -() -> int = 7
    pub operator ~() -> int = 7
    pub operator<int> ->() = 7
    pub operator ==(Item other) = true
    pub operator +(int n) -> int = n
    pub operator -(int n) -> int = n
    pub operator *(int n) -> int = n
    pub operator /(int n) -> int = n
    pub operator %(int n) -> int = n
}
typedef<class> Gauge(int seed) {
    pub Gauge() : (0) {}
    pub operator<int<auto>> +() -> int<auto> = 2
    pub operator<int<auto>> -() -> int<auto> = 2
    pub operator<int<auto>> *() -> int<auto> = 2
    pub operator<int<auto>> /() -> int<auto> = 2
    pub operator<int<auto>> %() -> int<auto> = 2
})";
	for (const std::string s_expression :
	     { "+Item::new()", "-Item::new()", "~Item::new()", "Item::new() + 2", "Item::new() - 2", "Item::new() * 2", "Item::new() / 2", "Item::new() % 2",
	       "40 + Gauge::new()", "Gauge::new() - 2", "6 * Gauge::new()", "84 / Gauge::new()", "142 % Gauge::new()" }) {
		SCOPED_TRACE(s_expression);
		ASSERT_NO_THROW(backend::LoweredModule(checkProgram("fn<int> main() = " + s_expression + s_declarations)).verify());
	}
	ASSERT_NO_THROW(backend::LoweredModule(checkProgram("fn<int> main() = Item::new() -> int" + s_declarations)).verify());
	ASSERT_NO_THROW(backend::LoweredModule(checkProgram("fn<int> main() = (Item::new() == Item::new()).toInt()" + s_declarations)).verify());
	const cloth::CheckedProgram cursor = checkProgram(R"(typedef<struct> Cursor { pub int at; pub operator +(int step) -> Cursor = {at + step} }
fn<int> main() { Cursor[1] a = [{7}]; a[0] += 2; return a[0].at })");
	ASSERT_NO_THROW(backend::LoweredModule(cursor).verify());
}

TEST (BackendLowering, ClassHandleWidthIsIndependentOfPointerWidth) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = checkProgram(R"(typedef<class> Marker { pub Marker(int tag) : () {} }
fn<Marker> make() = Marker::new(7)
fn<int> main() { Marker[] items = [make()]; return if (items[0] === items[0]) 42 else 1 })",
		                                                       { .sizeBits = u_bits });
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTypeRef ty_handle = LLVMInt64TypeInContext(LLVMGetModuleContext(lm_module.get()));
		for (const char *s_name : { "cloth_rt_object_create", "cloth.make" }) {
			const LLVMValueRef fn_make = LLVMGetNamedFunction(lm_module.get(), s_name);
			ASSERT_NE(fn_make, nullptr);
			EXPECT_EQ(LLVMGetReturnType(LLVMGlobalGetValueType(fn_make)), ty_handle);
		}
		const auto it_array = std::ranges::find_if(cp_program.localTypes, [] (const auto &local) { return local.second.array(); });
		ASSERT_NE(it_array, cp_program.localTypes.end());
		EXPECT_EQ(cp_program.arrayDataLayout(it_array->second, 2),
		          (cloth::StorageLayout{ 16, LLVMABIAlignmentOfType(LLVMGetModuleDataLayout(lm_module.get()), ty_handle) }));
	}
}

TEST (BackendLowering, EmitsOnlyReachableDispatchImplementations) {
	const char *source = R"(typedef<interface> Contract { pub mt<int> read() }
typedef<class> Ready : Contract {
    pub Ready(int n) : () {}
    pub mt<int> read() = 42
}
typedef<class> Unused : Contract {
    pub Unused(int n) : () {}
    pub mt<int> read() { print(1); return 0 }
}
fn<int> invoke(Contract value) = value.read()
fn<int> main() = invoke(Ready::new(0)))";
	for (const unsigned bits : { 32u, 64u }) {
		const auto                   program = checkProgram(source, { .sizeBits = bits });
		const backend::LoweredModule module(program);
		EXPECT_NE(LLVMGetNamedFunction(module.get(), "cloth.read"), nullptr);
		EXPECT_EQ(LLVMGetNamedFunction(module.get(), "cloth.read.1"), nullptr);
	}
}

TEST (BackendLowering, ChecksTheCapturedReceiverAroundArguments) {
	const char *source = R"(typedef<class> Item {
    pub Item(int value) : () {}
    pub mt<int> read(int n) = n
}
fn<int> invalidate(Item item) { item::delete; return 7 }
fn<int> use(Item item) = item.read(invalidate(item))
fn<int> main() = use(Item::new(42)))";
	for (const unsigned bits : { 32u, 64u }) {
		SCOPED_TRACE(bits);
		const cloth::CheckedProgram  program = checkProgram(source, { .sizeBits = bits });
		const backend::LoweredModule module(program);
		ASSERT_NO_THROW(module.verify());
		const LLVMValueRef use        = LLVMGetNamedFunction(module.get(), "cloth.use");
		const LLVMValueRef check      = LLVMGetNamedFunction(module.get(), "cloth_rt_object_data");
		const LLVMValueRef invalidate = LLVMGetNamedFunction(module.get(), "cloth.invalidate");
		const LLVMValueRef read       = LLVMGetNamedFunction(module.get(), "cloth.read");
		ASSERT_NE(use, nullptr);
		ASSERT_NE(check, nullptr);
		ASSERT_NE(invalidate, nullptr);
		ASSERT_NE(read, nullptr);
		std::vector<LLVMValueRef> calls, receivers;
		for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(use); block; block = LLVMGetNextBasicBlock(block)) {
			for (LLVMValueRef instruction = LLVMGetFirstInstruction(block); instruction; instruction = LLVMGetNextInstruction(instruction)) {
				if (!LLVMIsACallInst(instruction)) continue;
				calls.push_back(LLVMGetCalledValue(instruction));
				receivers.push_back(LLVMGetOperand(instruction, 0));
			}
		}
		ASSERT_EQ(calls, (std::vector<LLVMValueRef>{ check, invalidate, check, read }));
		EXPECT_EQ(receivers[0], receivers[2]);
		EXPECT_EQ(receivers[0], receivers[3]);
	}
}

TEST (BackendLowering, RejectsClassFieldsWhoseCombinedSizeExceedsTheTarget) {
	expectUnsupported(R"(typedef<class> Large(int seed) {
    pub def<int[536870912]> first = unused()
    pub def<int[536870912]> second = unused()
    pub Large() : (0) {}
}
fn<int[536870912]> unused() = exit(7)
fn<int> main() { Large::new(); return 42 })",
	                  Code::UnsupportedLowering, "Large", 1, { .sizeBits = 32 });
}

TEST (BackendLowering, ReusesClosedInstancesAcrossCallersAndRecursion) {
	const char *source = R"(fn<T> identity<T>(T n) = n
fn<T> step<T>(T n, int depth) = if (depth == 0) identity(n) else step(n, depth - 1)
fn<int<12>> narrow() = identity<int<12>>(42)
fn<int<12>> again() = identity<int<12>>(7)
fn<int> wide() = identity(11)
fn<T> unused<T>(T n) = n
fn<int> main() = (step(narrow(), 2) -> int) + (again() -> int) - wide())";
	for (const unsigned bits : { 32u, 64u }) {
		SCOPED_TRACE(bits);
		const cloth::CheckedProgram  program = checkProgram(source, { .sizeBits = bits });
		const backend::LoweredModule module(program);
		ASSERT_NO_THROW(module.verify());
		const auto calls = [] (LLVMValueRef function) {
			std::vector<LLVMValueRef> result;
			for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block; block = LLVMGetNextBasicBlock(block))
				for (LLVMValueRef inst = LLVMGetFirstInstruction(block); inst; inst = LLVMGetNextInstruction(inst))
					if (LLVMIsACallInst(inst)) result.push_back(LLVMGetCalledValue(inst));
			return result;
		};
		const auto narrow = calls(LLVMGetNamedFunction(module.get(), "cloth.narrow")), again = calls(LLVMGetNamedFunction(module.get(), "cloth.again")),
		           wide = calls(LLVMGetNamedFunction(module.get(), "cloth.wide"));
		ASSERT_EQ(narrow.size(), 1u);
		ASSERT_EQ(wide.size(), 1u);
		EXPECT_EQ(again, narrow);
		EXPECT_NE(narrow.front(), wide.front());
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMGetReturnType(LLVMGlobalGetValueType(narrow.front()))), 12u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMGetReturnType(LLVMGlobalGetValueType(wide.front()))), 32u);
		unsigned identities = 0;
		for (LLVMValueRef fn = LLVMGetFirstFunction(module.get()); fn; fn = LLVMGetNextFunction(fn)) {
			std::size_t length = 0;
			const char *name   = LLVMGetValueName2(fn, &length);
			if (std::string_view(name, length).starts_with("cloth.identity")) ++identities;
		}
		EXPECT_EQ(identities, 2u);
		const LLVMValueRef step = LLVMGetNamedFunction(module.get(), "cloth.step");
		ASSERT_NE(step, nullptr);
		const auto nested = calls(step);
		EXPECT_EQ(std::ranges::count(nested, step), 1);
		EXPECT_EQ(std::ranges::count(nested, narrow.front()), 1);
		EXPECT_EQ(LLVMGetNamedFunction(module.get(), "cloth.unused"), nullptr);
	}
}

TEST (BackendLowering, LabelsUnsupportedHelperSignatures) {
	expectUnsupported("fn<int> main() = helper([[42]])\nfn<int> helper(int[][] values) = 7", Code::UnsupportedFunctionAbi, "helper", 2);
	expectUnsupported("fn<int> main() { pair(); return 42 }\nfn<int, typeinfo> pair() = 1, int::type", Code::UnsupportedFunctionAbi, "pair", 2);
}

TEST (BackendLowering, AdaptsCheckedEntriesToTheNativeCSignature) {
	for (const unsigned u_bits : { 32u, 64u }) {
		for (const char *source :
		     { "fn<int> main(string[] args) = 42", "fn<int<12>> main(string[]? args) = 42", "fn<isize> main() = 42", "fn<void> main() {}" }) {
			SCOPED_TRACE(source);
			const cloth::CheckedProgram  program = checkProgram(source, { .sizeBits = u_bits });
			const backend::LoweredModule module(program);
			ASSERT_NO_THROW(module.verify());
			const LLVMValueRef native = LLVMGetNamedFunction(module.get(), "main");
			const LLVMValueRef cloth  = LLVMGetNamedFunction(module.get(), "cloth.main");
			ASSERT_NE(native, nullptr);
			ASSERT_NE(cloth, nullptr);
			EXPECT_EQ(LLVMGetLinkage(native), LLVMExternalLinkage);
			EXPECT_EQ(LLVMGetLinkage(cloth), LLVMInternalLinkage);
			EXPECT_EQ(LLVMGetIntTypeWidth(LLVMGetReturnType(LLVMGlobalGetValueType(native))), 32u);
			EXPECT_EQ(LLVMCountParams(native), LLVMCountParams(cloth) ? 2u : 0u);
		}
	}
}

TEST (BackendLowering, ReusesTheDeclaredDefaultAndPacksOrderedResults) {
	for (const unsigned u_bits : { 32u, 64u }) {
		const cloth::CheckedProgram  program = checkProgram(R"(
fn<int<12>, bool> pair(opt<int<12>> n ?: 42) = n, true
fn<int> main() { def a, ok := pair(); def b, _ := pair(); return if (ok) (a + b -> int) else 0 }
)",
		                                                    { .sizeBits = u_bits });
		const backend::LoweredModule module(program);
		ASSERT_NO_THROW(module.verify());
		const LLVMValueRef fn_pair    = LLVMGetNamedFunction(module.get(), "cloth.pair");
		const LLVMValueRef fn_default = LLVMGetNamedFunction(module.get(), "cloth.pair.default.0");
		ASSERT_NE(fn_pair, nullptr);
		ASSERT_NE(fn_default, nullptr);
		ASSERT_NE(LLVMGetFirstUse(fn_default), nullptr);
		EXPECT_NE(LLVMGetNextUse(LLVMGetFirstUse(fn_default)), nullptr);
		const LLVMTypeRef results = LLVMGetReturnType(LLVMGlobalGetValueType(fn_pair));
		ASSERT_EQ(LLVMGetTypeKind(results), LLVMStructTypeKind);
		ASSERT_EQ(LLVMCountStructElementTypes(results), 2u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(results, 0)), 12u);
	}
}

TEST (BackendLowering, RequiresAnEntrySelectedByTheFrontend) {
	const cloth::CheckedProgram cp_library = checkProgram("pub fn<int> main () = 42", {}, cloth::ProgramKind::Library);
	ASSERT_EQ(cp_library.entryPoint, nullptr);
	try {
		const backend::LoweredModule lm_module(cp_library);
		FAIL() << "Expected an executable compilation request";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.getCode(), Code::InvalidCompilationRequest) << error.what();
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
		EXPECT_EQ(error.getCode(), code) << error.what();
		EXPECT_NE(error.getMessage().find(detail), std::string::npos) << error.what();
	}
}

TEST (BackendObjectEmission, RejectsStructOffsetsAndSizesThatDisagreeWithCheckedFacts) {
	cloth::CheckedProgram cp_program = checkProgram(R"(typedef<struct> Inner { pub bool ready, pub int<12> value }
typedef<struct> Sample { pub bool tagged, pub Inner inner }
pub fn<int> main() { Sample sample := {true, {true, 42}}; return sample.inner.value -> int })");
	ASSERT_EQ(cp_program.localTypes.size(), 1u);
	cloth::StructLayout &sl_outer   = cp_program.structLayouts.at(cp_program.localTypes.begin()->second);
	cloth::StructLayout &sl_inner   = cp_program.structLayouts.at(sl_outer.fields.at(1).type);
	std::uint64_t       &u64_offset = sl_inner.fieldOffsets.at(sl_inner.fields.at(1).symbol);
	u64_offset                      = 1;
	expectBackendError(Code::AbiLayout, "requires byte 1", [&] { static_cast<void>(backend::emitObject(cp_program)); });
	u64_offset            = 2;
	sl_outer.storage.size = 7;
	expectBackendError(Code::AbiLayout, "requires size 7", [&] { static_cast<void>(backend::emitObject(cp_program)); });
}

TEST (BackendObjectEmission, RejectsUnknownTargetsAndEmbeddedNulls) {
	const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42");
	expectBackendError(Code::LlvmTarget, "cloth-unknown", [&] { static_cast<void>(backend::emitObject(cp_program, "cloth-unknown-none")); });
	const char s_triple[] = "x86_64-linux-gnu\0unused";
	expectBackendError(Code::LlvmTarget, "null byte",
	                   [&] { static_cast<void>(backend::emitObject(cp_program, std::string_view(s_triple, sizeof(s_triple) - 1))); });
}

TEST (BackendObjectEmission, ReportsVerifierFailures) {
	const backend::LoweredModule lm_module(checkProgram("pub fn<int> main () = 42"));
	const LLVMValueRef           fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
	LLVMInstructionEraseFromParent(LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn_main)));
	expectBackendError(Code::InvalidIr, "terminator", [&] { lm_module.verify(); });
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
	const TargetCase v_targets[] = { { .triple="i686-w64-windows-gnu", .layout={ .sizeBits=32, .scalar64Alignment32=8 }, .format=LLVMBinaryTypeCOFF, .symbol="_main" },
		                         { .triple="x86_64-w64-windows-gnu", .layout={ .sizeBits=64, .scalar64Alignment32=8 }, .format=LLVMBinaryTypeCOFF, .symbol="main" },
		                         { .triple="i386-unknown-linux-gnu", .layout={ .sizeBits=32, .scalar64Alignment32=4 }, .format=LLVMBinaryTypeELF32L, .symbol="main" },
		                         { .triple="x86_64-unknown-linux-gnu", .layout={ .sizeBits=64, .scalar64Alignment32=4 }, .format=LLVMBinaryTypeELF64L, .symbol="main" } };
	for (const TargetCase &tc_target : v_targets) {
		SCOPED_TRACE(tc_target.triple);
		const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main() { int answer = 42; answer = 7; return answer }", tc_target.layout);
		expectObject(backend::emitObject(cp_program, tc_target.triple), tc_target.format, tc_target.symbol);
#if defined(_WIN32)
		if (tc_target.format == LLVMBinaryTypeCOFF) expectObject(backend::emitObject(cp_program), tc_target.format, tc_target.symbol);
#elif defined(__linux__)
		if (tc_target.format != LLVMBinaryTypeCOFF) expectObject(backend::emitObject(cp_program), tc_target.format, tc_target.symbol);
#endif
	}
}

TEST (BackendObjectEmission, RejectsPointerWidthAndIntegerAlignmentMismatches) {
	const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42", { .sizeBits=32, .scalar64Alignment32=4 });
	expectBackendError(Code::AbiLayout, "64-bit pointers", [&] { static_cast<void>(backend::emitObject(cp_program, "x86_64-unknown-linux-gnu")); });
	expectBackendError(Code::AbiLayout, "i64", [&] { static_cast<void>(backend::emitObject(cp_program, "i686-w64-windows-gnu")); });
}
#endif

}
