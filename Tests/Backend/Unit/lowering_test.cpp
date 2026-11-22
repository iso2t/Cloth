// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>
#include <backend/lowered_module.hpp>

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
#include <cde/cloth_diagnostic.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Object.h>
#include <llvm-c/Target.h>
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
		expectReturn(checkProgram("pub fn<int> main () {\n    return 42\n}", { u_bits }), 42);
		expectReturn(checkProgram("fn<int> helper() { return 99 }\nfn<int<32>> main() = 0x7", { u_bits }), 7);
		expectReturn(checkProgram("fn<int> main() = 0", { u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = 2147483647", { u_bits }), 2147483647);
	}
}

TEST (BackendLowering, ReadsTheLiteralValueFromCheckedFacts) {
	for (const char *s_source : { "pub fn<int> main () = 42", "pub fn<int> main () { return 42 }" }) {
		SCOPED_TRACE(s_source);
		cloth::CheckedProgram cp_program = checkProgram(s_source);
		ASSERT_EQ(cp_program.expressions.size(), 1u);
		cp_program.expressions.begin()->second.numericConstant = cloth::IntegerConstant{ { 7 } };
		expectReturn(cp_program, 7);
	}
}

TEST (BackendLowering, ReturnsImmutableIntegerLocalsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("pub fn<int> main() { int answer := 42; return answer }", { u_bits }), 42);
		expectReturn(checkProgram("pub fn<int> main() { def answer := 7; return answer }", { u_bits }), 7);
		expectReturn(checkProgram("fn<int> main() { int<32> source := 42; def copy := source; int other := 7; return copy }", { u_bits }), 42);
		expectReturn(checkProgram("fn<int> main() { int caf\u00e9 := 7; return cafe\u0301 }", { u_bits }), 7);
	}
}

TEST (BackendLowering, UsesTheEnumCaseSelectedByTheFrontend) {
	std::ostringstream oss_source;
	oss_source << "typedef<enum> Mode { pub Zulu";
	for (unsigned u_index = 1; u_index < 256; ++u_index) oss_source << ", Hidden" << u_index;
	oss_source << ", pub Alpha }\nfn<int> main() = Mode.Zulu -> int";
	cloth::CheckedProgram     cp_program = checkProgram(oss_source.str());
	const cloth::SymbolScope &ss_cases   = cp_program.units.front()->get_globals().at("Mode").front().members;
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
		                                                    { u_bits }));
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
		                                                    { u_bits });
		const cloth::SymbolScope &ss_members = cp_program.units.front()->get_globals().at("Mode").front().members;
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

TEST (BackendLowering, NullablePrimitivesStoreAPresenceByteAndAnAlignedPayload) {
	for (const unsigned u_bits : { 32u, 64u }) {
		for (const std::string s_payload : { "int", "unsigned int<1>", "int<12>", "unsigned int<64>", "int<97>", "usize", "isize", "float<16>", "float",
		                                     "float<64>", "bool", "char" }) {
			SCOPED_TRACE(::testing::Message() << s_payload << " on " << u_bits << "-bit target");
			const std::string            s_type       = s_payload + "?";
			const cloth::CheckedProgram  cp_program   = checkProgram("fn<int> main() = if (identity(null) == null) 42 else 7\nfn<" + s_type +
			                                                                 "> identity(final " + s_type + " value) = value",
			                                                         { u_bits });
			const cloth::Symbol         &sym_identity = cp_program.units.front()->get_globals().at("identity").front();
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
		                     { u_bits });
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
		                                                    { u_bits }));
		ASSERT_NO_THROW(lm_module.verify());
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());

		struct FieldCase {
			const char *function;
			unsigned    width, offset, size, alignment;
		};

		const unsigned u_word = u_bits / 8;
		for (const FieldCase fc_case : { FieldCase{ "cloth.small", 12, 2, 4, 2 }, FieldCase{ "cloth.sized", u_bits, u_word, 2 * u_word, u_word } }) {
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

TEST (BackendLowering, EmbedsNestedStructFieldsAtTheirTargetAlignment) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(typedef<struct> Inner<T> { pub bool ready, pub T word, pub int<12> value }
typedef<struct> Sample { pub bool first, pub Inner<usize> inner, pub bool last }
fn<int> main() { Sample sample = {false, {true, 1, 42}, false}; sample.inner.value = 7; return sample.inner.value -> int })",
		                                                    { u_bits }));
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
		                                                     { u_bits });
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
		if (lt_local.first->get_lexeme() == "second") t_second = lt_local.first;
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
		const backend::LoweredModule lm_module(checkProgram("pub fn<int> main() { int answer = 42; answer = 7; return answer }", { u_bits }));
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
		if (lt_local.first->get_lexeme() == "second") t_second = lt_local.first;
	}
	ASSERT_NE(t_second, nullptr);
	for (decltype(cp_program.references)::value_type &ref_use : cp_program.references) {
		if (ref_use.first->get_lexeme() == "first") ref_use.second = t_second;
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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

TEST (BackendLowering, LeavesEndlessLoopsWithoutAnImplicitReturn) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		for (const char *s_source : { "pub fn<int> main() { while (true) {} }", "pub fn<int> main() { do {} while (true) }" }) {
			SCOPED_TRACE(s_source);
			const backend::LoweredModule lm_module(checkProgram(s_source, { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		        checkProgram("pub fn<int> main() { do { return 42 } while (unused()) }\nfn<bool> unused() = true", { u_bits }));
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
		                                                { u_bits });
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
		                                                    { u_bits }));
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
			const backend::LoweredModule lm_module(checkProgram(s_source, { u_bits }));
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
		const backend::LoweredModule lm_module(checkProgram("pub fn<int> main() { touch(); return 42 }\nfn<void> touch() {}", { u_bits }));
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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

TEST (BackendLowering, OmitsTheExitRuntimeWhenTheStatusReturnsFromTheFunction) {
	const backend::LoweredModule lm_module(checkProgram(R"(fn<int> main() = exit(if (true) { return 42 } else { return 7 }, "unreached"))"));
	ASSERT_NO_THROW(lm_module.verify());
	EXPECT_EQ(LLVMGetNamedFunction(lm_module.get(), "cloth_rt_exit"), nullptr);
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
		                                              { u_bits }),
		                                 "cloth.add", LLVMAdd);
		expectReturn(checkProgram("fn<int> main() = 2147483647 + 1", { u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, SubtractsTwoIntegerParametersWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectWrappingParameterOperation(
		        checkProgram("pub fn<int> main() = subtract(44, 2)\nfn<int> subtract(final int left, final int right) = left - right", { u_bits }),
		        "cloth.subtract", LLVMSub);
		expectReturn(checkProgram("fn<int> main() = (0 - 2147483647 - 1) - 1", { u_bits }), 2147483647u);
	}
}

TEST (BackendLowering, MultipliesTwoIntegerParametersWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectWrappingParameterOperation(
		        checkProgram("pub fn<int> main() = multiply(6, 7)\nfn<int> multiply(final int left, final int right) = left * right", { u_bits }),
		        "cloth.multiply", LLVMMul);
		expectReturn(checkProgram("fn<int> main() = 2147483647 * 2", { u_bits }), 0xfffffffeu);
		expectReturn(checkProgram("fn<int> main() = 65536 * 65536", { u_bits }), 0u);
	}
}

TEST (BackendLowering, NegatesAnIntegerOperandOnceWithWrappingSemanticsOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = negate(-42)
fn<int> negate(final int value) = -operand(value)
fn<int> operand(final int value) = value
)",
		                                                    { u_bits }));
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
		expectReturn(checkProgram("fn<int> main() = -42", { u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = -(-42)", { u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = -0", { u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = -2147483648", { u_bits }), 0x80000000u);
		expectReturn(checkProgram("fn<int> main() = -(-2147483648)", { u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, UnaryPlusReturnsItsOperandDirectlyOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(
		        checkProgram("pub fn<int> main() = +positive(42)\nfn<int> positive(final int value) = +value", { u_bits }));
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
		expectReturn(checkProgram("fn<int> main() = +42", { u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = +(-42)", { u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = +0", { u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = +2147483647", { u_bits }), 0x7fffffffu);
		expectReturn(checkProgram("fn<int> main() = +(-2147483648)", { u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, ComplementsAnIntegerOperandOnceOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = invert(-43)
fn<int> invert(final int value) = ~operand(value)
fn<int> operand(final int value) = value
)",
		                                                    { u_bits }));
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
		expectReturn(checkProgram("fn<int> main() = ~0", { u_bits }), 0xffffffffu);
		expectReturn(checkProgram("fn<int> main() = ~(-1)", { u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = ~2147483647", { u_bits }), 0x80000000u);
		expectReturn(checkProgram("fn<int> main() = ~(-2147483648)", { u_bits }), 0x7fffffffu);
		expectReturn(checkProgram("fn<int> main() = ~~42", { u_bits }), 42u);
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
		                                                    { u_bits }));
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
		                                                    { u_bits }));
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
		expectReturn(checkProgram("pub fn<int> main() = 42.75 -> int", { u_bits }), 42u);
		const backend::LoweredModule lm_module(checkProgram(R"(pub fn<int> main() = (asSigned(42.75) -> int) + (asUnsigned(0.5) -> int)
fn<isize> asSigned(final float<64> value) = value -> isize
fn<usize> asUnsigned(final float<64> value) = value -> usize
)",
		                                                    { u_bits }));
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
		expectReturn(checkProgram("fn<int> main() = 85 / 2", { u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (0 - 85) / 2", { u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = 85 / (0 - 2)", { u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = (0 - 85) / (0 - 2)", { u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (2147483647 + 1) / 1", { u_bits }), 0x80000000u);
	}
}

TEST (BackendLowering, SignedRemaindersHaveTheSignOfTheDividendOnBothTargetWidths) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		expectReturn(checkProgram("fn<int> main() = 142 % 100", { u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (0 - 142) % 100", { u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = 142 % (0 - 100)", { u_bits }), 42u);
		expectReturn(checkProgram("fn<int> main() = (0 - 142) % (0 - 100)", { u_bits }), 0xffffffd6u);
		expectReturn(checkProgram("fn<int> main() = 142 % (0 - 1)", { u_bits }), 0u);
		expectReturn(checkProgram("fn<int> main() = (2147483647 + 1) % 3", { u_bits }), 0xfffffffeu);
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
	const cloth::SymbolScope &ss_globals   = cp_program.units.front()->get_globals();
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

void expectUnsupported (const std::string &source, Code code, std::string_view markedText, std::size_t line = 1) {
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
		        { u_bits }));
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
		expectReturn(checkProgram("fn<int> main() { bool b := 0; return b.toInt() }", { u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = (false == 1).toInt()", { u_bits }), 0);
		expectReturn(checkProgram("fn<int> main() = (true == -2).toInt()", { u_bits }), 1);
		expectReturn(checkProgram("fn<int> main() = (int<65536>::max + 1 == int<65536>::min).toInt()", { u_bits }), 1);
		expectReturn(checkProgram("fn<int> main() = (unsigned int<1>::max + 1 == 0).toInt()", { u_bits }), 1);
		expectReturn(checkProgram("fn<int> main() = isize::size -> int", { u_bits }), u_bits / 8);
		expectReturn(checkProgram("fn<int> main() = ((-1 -> int<12>) -> int<40>) -> int", { u_bits }), 0xffffffffu);
		expectReturn(checkProgram("fn<int> main() = ((-1 -> unsigned int<12>) -> int<40>) -> int", { u_bits }), 4095);
	}
}

TEST (BackendLowering, LabelsUnsupportedStatementsInSourceOrder) {
	expectUnsupported("fn<int> main() { def stop := exit(7); return 42 }", Code::UNSUPPORTED_LOWERING, "return 42");
	expectUnsupported("pub fn<int> main() { print(1); return 42 }", Code::UNSUPPORTED_LOWERING, "print(1)");
	expectUnsupported("pub fn<int> main() { return 42; println(1) }", Code::UNSUPPORTED_LOWERING, "println(1)");
	expectUnsupported("pub fn<int> main() { return 42; return 7 }", Code::UNSUPPORTED_LOWERING, "return 7");
	expectUnsupported("fn<int> main() { int a := 42; print(a); return a }", Code::UNSUPPORTED_LOWERING, "print(a)");
	expectUnsupported("fn<int> main() { return 42; int a := 7 }", Code::UNSUPPORTED_LOWERING, "int a := 7");
	expectUnsupported("fn<int> main() { int a = 42; return a; a = 7 }", Code::UNSUPPORTED_LOWERING, "a = 7");
	expectUnsupported("fn<int> main() { touch(); return 42 }\nfn<void> touch() { return; other() }\nfn other() {}", Code::UNSUPPORTED_LOWERING, "other()",
	                  2);
	expectUnsupported("fn<int> main() { if (true) { return 42; println(1) } else { return 7 } }", Code::UNSUPPORTED_LOWERING, "println(1)");
	expectUnsupported("fn<int> main() { if (false) { return 42 } else { return 7 }; println(1) }", Code::UNSUPPORTED_LOWERING, "println(1)");
	expectUnsupported("fn<int> main() { while (true) {}; return 7 }", Code::UNSUPPORTED_LOWERING, "return 7");
	expectUnsupported("fn<int> main() { do {} while (true); return 7 }", Code::UNSUPPORTED_LOWERING, "return 7");
	expectUnsupported("fn<int> main() { while (true) { break; println(1) }; return 7 }", Code::UNSUPPORTED_LOWERING, "println(1)");
	expectUnsupported("fn<int> main() { do { continue; return 7 } while (false); return 42 }", Code::UNSUPPORTED_LOWERING, "return 7");
}

TEST (BackendLowering, LabelsUnsupportedConditionalArmExpressions) {
	expectUnsupported("fn<int> main() { if (true) { print(1) }; return 7 }", Code::UNSUPPORTED_LOWERING, "print(1)");
	expectUnsupported("fn<int> main() = if (true) 42 else [7][0]", Code::UNSUPPORTED_LOWERING, "[7][0]");
}

TEST (BackendLowering, LabelsUnsupportedLocalBindingsAndInitializers) {
	expectUnsupported(R"(typedef<struct> S { pub string text, pub int value }
fn<int> main() { S s = {"text", 42}; s.value = 7; return s.value })",
	                  Code::UNSUPPORTED_LOWERING, "S", 2);
	expectUnsupported(R"(typedef<struct> S { pub string text, pub int value }
fn<int> main() { S s := {"text", 42}; return s.value })",
	                  Code::UNSUPPORTED_LOWERING, "S", 2);
	expectUnsupported(R"(typedef<struct> Leaf { pub string text }
typedef<struct> Box { pub Leaf item }
fn<int> main() { Box value = {{"text"}}; return 7 })",
	                  Code::UNSUPPORTED_LOWERING, "Box", 3);
	expectUnsupported("fn<int> main() { string? text = null; return 42 }", Code::UNSUPPORTED_LOWERING, "string?");
	expectUnsupported("fn<int> main() { int[] a := [7]; return 42 }", Code::UNSUPPORTED_LOWERING, "int[]");
	expectUnsupported("fn<int> main() { int unused := [40, 2][0]; return 42 }", Code::UNSUPPORTED_LOWERING, "[40, 2][0]");
	expectUnsupported("fn<int> main() { int unused := helper([40, 2][0]); return 42 }\nfn<int> helper(int ignored) = 7", Code::UNSUPPORTED_LOWERING,
	                  "[40, 2][0]");
	expectUnsupported("fn<int> main() { int later := { return 7 }; return later }", Code::UNSUPPORTED_LOWERING, "{ return 7 }");
}

TEST (BackendLowering, LabelsUnsupportedAssignments) {
	expectUnsupported("fn<int> main() { int a = 42; a += [7][0]; return a }", Code::UNSUPPORTED_LOWERING, "[7][0]");
	expectUnsupported("fn<int> main() { int a = 42; a = [7][0]; a = 7; return a }", Code::UNSUPPORTED_LOWERING, "[7][0]");
}

TEST (BackendLowering, LabelsUnsupportedExpressions) {
	expectUnsupported(R"(fn<int> main() = exit(7, "finished"))", Code::UNSUPPORTED_LOWERING, "\"finished\"");
	expectUnsupported("pub fn<int> main() = 40 - [2][0]", Code::UNSUPPORTED_LOWERING, "[2][0]");
	expectUnsupported("pub fn<int> main() = [2][0] + (if (true) { return 42 } else 0)", Code::UNSUPPORTED_LOWERING, "[2][0]");
	expectUnsupported("pub fn<int> main() = +[2][0]", Code::UNSUPPORTED_LOWERING, "[2][0]");
	expectUnsupported("pub fn<int> main() = ~[2][0] ** 2", Code::UNSUPPORTED_LOWERING, "[2][0]");
	expectUnsupported("pub fn<int> main() = sideEffect()\nfn<int> sideEffect() { print(1)\nreturn 42 }", Code::UNSUPPORTED_LOWERING, "print(1)", 2);
}

TEST (BackendLowering, LabelsUnsupportedPayloadFieldTypesAndUserDefinedEnumEquality) {
	const std::string s_payload = "typedef<enum> Mode(pub def<int[2]> row = [1, 2]) { pub On }\n";
	expectUnsupported(s_payload + "fn<int> main() { Mode mode = Mode.On; mode.row; return 42 }", Code::UNSUPPORTED_LOWERING, "mode.row", 2);
	const std::string s_override = "typedef<enum> Mode { pub Off, pub On; pub operator ==(Mode other) -> bool = true }\n";
	expectUnsupported(s_override + "fn<int> main() = if (Mode.On == Mode.Off) 42 else 7", Code::UNSUPPORTED_LOWERING, "Mode.On == Mode.Off", 2);
	expectUnsupported(s_override + "fn<int> main() = if (Mode.On != Mode.Off) 42 else 7", Code::UNSUPPORTED_LOWERING, "Mode.On != Mode.Off", 2);
}

TEST (BackendLowering, LabelsUnsupportedPrintlnForms) {
	for (const std::string s_call : { "println()", "println(true)", "println(1 -> int<64>)", "println(1 -> unsigned int)", "println(\"1\")" }) {
		expectUnsupported("fn<int> main() { " + s_call + "; return 42 }", Code::UNSUPPORTED_LOWERING, s_call);
	}
}

TEST (BackendLowering, LabelsArithmeticThatRequiresAUserDefinedOperator) {
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
		expectUnsupported("fn<int> main() = " + s_expression + s_declarations, Code::UNSUPPORTED_LOWERING, s_expression);
	}
	expectUnsupported("fn<int> main() = Item::new() -> int" + s_declarations, Code::UNSUPPORTED_LOWERING, "Item::new() -> int");
	expectUnsupported("fn<int> main() = (Item::new() == Item::new()).toInt()" + s_declarations, Code::UNSUPPORTED_LOWERING, "Item::new() == Item::new()");
}

TEST (BackendLowering, LabelsUnsupportedCallFormsAndHelperSignatures) {
	expectUnsupported("fn<int> main() = helper()\nfn<int> helper(opt<int> n ?: 7) = n", Code::UNSUPPORTED_LOWERING, "helper()");
	expectUnsupported("fn<int> main() = identity(42)\nfn<T> identity<T>(T value) = value", Code::UNSUPPORTED_LOWERING, "identity(42)");
	expectUnsupported("fn<int> main() = helper([42])\nfn<int> helper(int[] values) = 7", Code::UNSUPPORTED_FUNCTION_ABI, "helper", 2);
	expectUnsupported("fn<int> main() { helper([42]); return 42 }\nfn<void> helper(int[] values) {}", Code::UNSUPPORTED_FUNCTION_ABI, "helper", 2);
	expectUnsupported("fn<int> main() { pair(); return 42 }\nfn<int, int> pair() = 1, 2", Code::UNSUPPORTED_FUNCTION_ABI, "pair", 2);
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

TEST (BackendObjectEmission, RejectsStructOffsetsAndSizesThatDisagreeWithCheckedFacts) {
	cloth::CheckedProgram cp_program = checkProgram(R"(typedef<struct> Inner { pub bool ready, pub int<12> value }
typedef<struct> Sample { pub bool tagged, pub Inner inner }
pub fn<int> main() { Sample sample := {true, {true, 42}}; return sample.inner.value -> int })");
	ASSERT_EQ(cp_program.localTypes.size(), 1u);
	cloth::StructLayout &sl_outer   = cp_program.structLayouts.at(cp_program.localTypes.begin()->second);
	cloth::StructLayout &sl_inner   = cp_program.structLayouts.at(sl_outer.fields.at(1).type);
	std::uint64_t       &u64_offset = sl_inner.fieldOffsets.at(sl_inner.fields.at(1).symbol);
	u64_offset                      = 1;
	expectBackendError(Code::ABI_LAYOUT, "requires byte 1", [&] { static_cast<void>(backend::emitObject(cp_program)); });
	u64_offset            = 2;
	sl_outer.storage.size = 7;
	expectBackendError(Code::ABI_LAYOUT, "requires size 7", [&] { static_cast<void>(backend::emitObject(cp_program)); });
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
	const cloth::CheckedProgram cp_program = checkProgram("pub fn<int> main () = 42", { 32, 4 });
	expectBackendError(Code::ABI_LAYOUT, "64-bit pointers", [&] { static_cast<void>(backend::emitObject(cp_program, "x86_64-unknown-linux-gnu")); });
	expectBackendError(Code::ABI_LAYOUT, "i64", [&] { static_cast<void>(backend::emitObject(cp_program, "i686-w64-windows-gnu")); });
}
#endif

}
