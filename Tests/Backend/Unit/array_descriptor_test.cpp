// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/array_descriptor.hpp>
#include <backend/lowered_module.hpp>
#include <backend/value_types.hpp>

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>

#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/program_kind.hpp>
#include <semantic/symbol_table.hpp>

namespace {

cloth::CheckedProgram descriptorProgram (unsigned    bits,
                                         std::string source = R"(typedef<struct> Record { pub bool enabled, pub int<12>[2][0] rows, pub usize stamp }
fn shapes(Record record, int[] growable, int[]?[] nullableRows, Record[1] records) {}
pub fn<int> main() { Record value = {false, [[], []], 42}; return value.stamp -> int })") {
	cloth::Lexer l_lexer{ cloth::SourceFile("entry.co", std::move(source)) };
	return cloth::CheckedProgram(cloth::SymbolTable(cloth::Parser(l_lexer).parse()), { .sizeBits = bits }, {}, cloth::ProgramKind::Executable);
}

TEST (BackendArrayDescriptor, FieldAndShapeOffsetsFollowTheCheckedTarget) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = descriptorProgram(u_bits);
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		backend::detail::ValueTypes vt_types(lm_module.get(), cp_program);
		const cloth::ResultTypes   &v_types   = cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shapes").front()).parameters;
		const cloth::StructLayout  &sl_record = cp_program.structLayouts.at(v_types[0]);
		const LLVMTypeRef           ty_record = vt_types.get(v_types[0]);
		const LLVMTypeRef           ty_value  = vt_types.localType(v_types[0]);
		ASSERT_NE(ty_record, nullptr);
		ASSERT_NE(ty_value, nullptr);
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());
		const unsigned          u_word    = u_bits / 8;
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_record), sl_record.storage.size);
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_record, 1), sl_record.fieldOffsets.at(sl_record.fields[1].symbol));
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_record, 2), 7u * u_word);
		EXPECT_EQ(LLVMGetTypeKind(LLVMStructGetTypeAtIndex(ty_value, 1)), LLVMArrayTypeKind);
		const LLVMTypeRef ty_descriptor = LLVMStructGetTypeAtIndex(ty_record, 1);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_descriptor), 5u);
		EXPECT_EQ(LLVMGetTypeKind(LLVMStructGetTypeAtIndex(ty_descriptor, 0)), LLVMPointerTypeKind);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_descriptor, 1)), u_bits);
		const LLVMTypeRef ty_extent = LLVMStructGetTypeAtIndex(ty_descriptor, 2);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_extent), 2u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_extent, 0)), u_bits);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_extent, 1)), 1u);
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_descriptor, 3), 4u * u_word);
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_descriptor, 4), 5u * u_word);

		const LLVMTypeRef ty_dynamic = backend::detail::array_descriptor::type(lm_module.get(), cp_program, v_types[1]);
		const LLVMTypeRef ty_shared  = backend::detail::array_descriptor::type(lm_module.get(), cp_program, v_types[2]);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_dynamic), 4u * u_word);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_shared), 6u);
		EXPECT_EQ(LLVMGetTypeKind(LLVMStructGetTypeAtIndex(ty_shared, 3)), LLVMPointerTypeKind);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_shared), 7u * u_word);
		EXPECT_EQ(vt_types.localType(v_types[1]), ty_dynamic);
		EXPECT_EQ(vt_types.localType(v_types[2]), nullptr);
		const LLVMTypeRef ty_records = vt_types.slotType(v_types[3]);
		ASSERT_NE(ty_records, nullptr);
		EXPECT_EQ(LLVMGetElementType(ty_records), ty_record);
		EXPECT_EQ(LLVMGetElementType(vt_types.localType(v_types[3])), ty_value);
		const cloth::StorageLayout sl_records{ .size      = LLVMABISizeOfType(td_target, ty_records),
			                               .alignment = LLVMABIAlignmentOfType(td_target, ty_records) };
		EXPECT_EQ(cp_program.arrayDataLayout(v_types[3], 1), sl_records);
	}
}

TEST (BackendArrayDescriptor, FixedFieldPointsAtItsOwnBufferAndRetainsKnownEmptyRows) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = descriptorProgram(u_bits);
		const backend::LoweredModule lm_module(cp_program);
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		const cloth::ResolvedType   &rt_record =
		        cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shapes").front()).parameters.front();
		const LLVMTypeRef  ty_descriptor = LLVMStructGetTypeAtIndex(vt_types.get(rt_record), 1);
		const LLVMValueRef fn_main       = LLVMGetNamedFunction(lm_module.get(), "main");
		LLVMValueRef       ir_descriptor = nullptr;
		for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main)); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
			if (LLVMIsAStoreInst(ir_inst) && LLVMTypeOf(LLVMGetOperand(ir_inst, 0)) == ty_descriptor) ir_descriptor = LLVMGetOperand(ir_inst, 0);
		}
		ASSERT_NE(ir_descriptor, nullptr);
		ASSERT_NE(LLVMIsAInsertValueInst(ir_descriptor), nullptr);
		EXPECT_NE(LLVMIsAAllocaInst(LLVMGetOperand(ir_descriptor, 1)), nullptr);
		const LLVMValueRef ir_header = LLVMGetOperand(ir_descriptor, 0);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_header, 1)), 2u);
		const LLVMValueRef ir_extent = LLVMGetAggregateElement(ir_header, 2);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_extent, 0)), 0u);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_extent, 1)), 1u);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_header, 3)), 2u);
		EXPECT_TRUE(LLVMIsNull(LLVMGetAggregateElement(ir_header, 4)));
	}
}

TEST (BackendArrayDescriptor, AggregateSnapshotsMustFitTheTarget) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const std::uint64_t u64_chunk = std::uint64_t{ 1 } << (u_bits - 4);
		std::string         s_source  = "typedef<struct> Part { int<8>[" + std::to_string(u64_chunk) + "] cells }\ntypedef<struct> Whole { ";
		for (unsigned u_index = 0; u_index < 16; ++u_index) {
			if (u_index) s_source += ", ";
			s_source += "Part part" + std::to_string(u_index);
		}
		s_source += " }\nfn shapes(Part part, Whole whole) {}\npub fn<int> main() = 42";
		const cloth::CheckedProgram  cp_program = descriptorProgram(u_bits, std::move(s_source));
		const backend::LoweredModule lm_module(cp_program);
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		const cloth::ResultTypes    &v_types = cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shapes").front()).parameters;
		ASSERT_TRUE(cp_program.storageLayout(v_types[1]).has_value());
		const LLVMTypeRef ty_part = vt_types.localType(v_types[0]);
		ASSERT_NE(ty_part, nullptr);
		EXPECT_EQ(LLVMABISizeOfType(LLVMGetModuleDataLayout(lm_module.get()), ty_part), u64_chunk);
		EXPECT_EQ(vt_types.localType(v_types[1]), nullptr);
	}
}

TEST (BackendArrayDescriptor, NullableFieldsAppendIndependentPointerFreeShapeWitnesses) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram cp_program =
		        descriptorProgram(u_bits, R"(typedef<struct> Record { pub int<12>[2][0]? rows, pub usize stamp, pub int[3]? cells }
fn shape(Record value) {}
pub fn<int> main() { Record value = {[[], []], 42, null}; value.rows = null; Record copy = value; return copy.stamp -> int })");
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		backend::detail::ValueTypes vt_types(lm_module.get(), cp_program);
		const cloth::ResolvedType  &rt_record =
		        cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shape").front()).parameters.front();
		const LLVMTypeRef ty_stored = vt_types.get(rt_record), ty_value = vt_types.localType(rt_record);
		ASSERT_NE(ty_stored, nullptr);
		ASSERT_NE(ty_value, nullptr);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_stored), 5u);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_value), 5u);
		const LLVMTargetDataRef td_target = LLVMGetModuleDataLayout(lm_module.get());
		const unsigned          u_word    = u_bits / 8;
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_stored, 3), 13u * u_word);
		EXPECT_EQ(LLVMOffsetOfElement(td_target, ty_stored, 4), 20u * u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_stored), 25u * u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_target, ty_stored), cp_program.structLayouts.at(rt_record).storage.size);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_stored, 3), LLVMStructGetTypeAtIndex(ty_stored, 0));
		EXPECT_EQ(LLVMStructGetTypeAtIndex(ty_stored, 4), LLVMStructGetTypeAtIndex(ty_stored, 2));
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_value, 3)), 8u);
		EXPECT_EQ(LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(ty_value, 4)), 8u);

		std::array<LLVMValueRef, 2> a_initial{};
		std::array<unsigned, 2>     a_stores{}, a_reads{};
		const LLVMValueRef          fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(LLVMGetEntryBasicBlock(fn_main)); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
			const bool b_store = LLVMIsAStoreInst(ir_inst) != nullptr;
			if (!b_store && !LLVMIsALoadInst(ir_inst)) continue;
			const LLVMValueRef ir_address = LLVMGetOperand(ir_inst, b_store ? 1 : 0);
			if (!LLVMIsAGetElementPtrInst(ir_address) || LLVMGetGEPSourceElementType(ir_address) != ty_stored ||
			    LLVMGetNumOperands(ir_address) != 3)
				continue;
			const std::uint64_t u64_field = LLVMConstIntGetZExtValue(LLVMGetOperand(ir_address, 2));
			if (u64_field < 3) continue;
			const auto u_witness = static_cast<unsigned>(u64_field - 3);
			ASSERT_LT(u_witness, a_stores.size());
			if (b_store) {
				if (a_stores[u_witness]++ == 0) a_initial[u_witness] = LLVMGetOperand(ir_inst, 0);
			} else {
				++a_reads[u_witness];
			}
		}
		// Both fields are copied. Only rows is independently cleared, which also reads its retained tag.
		EXPECT_EQ(a_stores[0], 3u);
		EXPECT_EQ(a_stores[1], 2u);
		EXPECT_GE(a_reads[0], 2u);
		EXPECT_GE(a_reads[1], 1u);
		ASSERT_NE(a_initial[0], nullptr);
		ASSERT_NE(a_initial[1], nullptr);
		EXPECT_TRUE(LLVMIsNull(a_initial[1]));
		ASSERT_TRUE(LLVMIsConstant(a_initial[0]));
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(a_initial[0], 0)), 1u);
		const LLVMValueRef ir_shape = LLVMGetAggregateElement(a_initial[0], 1);
		EXPECT_TRUE(LLVMIsNull(LLVMGetAggregateElement(ir_shape, 0)));
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_shape, 1)), 2u);
		const LLVMValueRef ir_inner = LLVMGetAggregateElement(ir_shape, 2);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_inner, 0)), 0u);
		EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_inner, 1)), 1u);
		EXPECT_TRUE(LLVMIsNull(LLVMGetAggregateElement(ir_shape, 3)));
		EXPECT_TRUE(LLVMIsNull(LLVMGetAggregateElement(ir_shape, 4)));
	}
}

TEST (BackendArrayDescriptor, StructArrayCopiesUseOneBackingBlockPerFieldPathAndDestination) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = descriptorProgram(u_bits, R"(typedef<struct> Cell { pub int<12>[2]? cells, pub usize stamp }
fn shapes(Cell?[3][2] small, Cell?[3][4096] large, Cell?[268435455][0] empty) {}
pub fn<int> main() = 42)");
		const backend::LoweredModule lm_module(cp_program);
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		const cloth::ResultTypes    &v_types     = cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shapes").front()).parameters;
		const LLVMContextRef         ctx_context = LLVMGetModuleContext(lm_module.get());
		const LLVMTargetDataRef      td_target   = LLVMGetModuleDataLayout(lm_module.get());
		const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_builder(LLVMCreateBuilderInContext(ctx_context), LLVMDisposeBuilder);
		std::array<unsigned, 3>                                                 a_instructions{}, a_allocations{}, a_loops{}, a_backings{};
		for (unsigned u_case = 0; const cloth::ResolvedType &rt_array : v_types) {
			const LLVMTypeRef ty_stored = vt_types.slotType(rt_array), ty_value = vt_types.localType(rt_array);
			ASSERT_NE(ty_stored, nullptr);
			ASSERT_NE(ty_value, nullptr);
			EXPECT_NE(ty_stored, ty_value);
			const std::uint64_t u64_rows = LLVMGetArrayLength2(ty_stored), u64_cols = LLVMGetArrayLength2(LLVMGetElementType(ty_stored));
			EXPECT_EQ(rt_array.arrayExtent(), u64_rows);
			EXPECT_EQ(rt_array.element().arrayExtent(), u64_cols);
			const cloth::StorageLayout sl_stored{ .size      = LLVMABISizeOfType(td_target, ty_stored),
				                              .alignment = LLVMABIAlignmentOfType(td_target, ty_stored) };
			EXPECT_EQ(cp_program.arrayDataLayout(rt_array, u64_rows), sl_stored);
			const LLVMTypeRef       ty_cells   = LLVMArrayType2(LLVMIntTypeInContext(ctx_context, 12), 2);
			const LLVMTypeRef       ty_backing = LLVMArrayType2(LLVMArrayType2(ty_cells, u64_cols), u64_rows);
			const LLVMTypeRef       ty_probe   = LLVMFunctionType(LLVMVoidTypeInContext(ctx_context), nullptr, 0, false);
			const LLVMValueRef      fn_probe   = LLVMAddFunction(lm_module.get(), "probe", ty_probe);
			const LLVMBasicBlockRef bb_entry   = LLVMAppendBasicBlockInContext(ctx_context, fn_probe, "entry");
			LLVMPositionBuilderAtEnd(ir_builder.get(), bb_entry);
			const LLVMValueRef ir_source = vt_types.allocateLocal(ir_builder.get(), rt_array, LLVMConstNull(ty_value));
			const LLVMValueRef ir_copy =
			        vt_types.allocateLocal(ir_builder.get(), rt_array, vt_types.loadLocal(ir_builder.get(), rt_array, ir_source));
			vt_types.storeLocal(ir_builder.get(), rt_array, ir_source, vt_types.loadLocal(ir_builder.get(), rt_array, ir_copy));
			LLVMBuildRetVoid(ir_builder.get());
			for (LLVMBasicBlockRef bb_block = bb_entry; bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
				for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
					++a_instructions[u_case];
					if (LLVMIsAPHINode(ir_inst)) ++a_loops[u_case];
					if (LLVMIsAAllocaInst(ir_inst)) {
						EXPECT_EQ(bb_block, bb_entry);
						++a_allocations[u_case];
						if (LLVMGetAllocatedType(ir_inst) == ty_backing) ++a_backings[u_case];
					}
				}
			}
			++u_case;
		}
		ASSERT_NO_THROW(lm_module.verify());
		EXPECT_EQ(a_backings[0], 2u);
		EXPECT_EQ(a_backings[1], 2u);
		EXPECT_EQ(a_allocations[0], a_allocations[1]);
		EXPECT_EQ(a_instructions[0], a_instructions[1]);
		EXPECT_GT(a_loops[0], 0u);
		EXPECT_EQ(a_loops[2], 0u);
		EXPECT_EQ(a_backings[2], 0u);
	}
}

TEST (BackendArrayDescriptor, BackingBlocksMustFitAcrossAllEnclosingExtents) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const std::uint64_t         u64_chunk  = std::uint64_t{ 1 } << (u_bits - 4);
		const std::uint64_t         u64_count  = u64_chunk / (std::uint64_t{ 4 } * (u_bits / 8));
		const cloth::CheckedProgram cp_program = descriptorProgram(
		        u_bits,
		        "typedef<struct> Cell { int<8>[0] cells }\ntypedef<struct> Branch { Cell[" + std::to_string(u64_count) +
		                "] items }\nfn shapes(Branch item, Branch[16] tooLarge, Branch[0] empty, Branch[16][0] rows) {}\npub fn<int> main() = 42");
		const backend::LoweredModule lm_module(cp_program);
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		const cloth::ResultTypes    &v_types  = cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shapes").front()).parameters;
		const cloth::ResolvedType   &rt_items = cp_program.structLayouts.at(v_types[0]).fields.front().type;
		ASSERT_TRUE(cp_program.arrayDataLayout(v_types[1], 16).has_value());
		ASSERT_NE(vt_types.slotType(rt_items), nullptr);
		EXPECT_EQ(LLVMABISizeOfType(LLVMGetModuleDataLayout(lm_module.get()), vt_types.slotType(rt_items)), u64_chunk);
		EXPECT_EQ(vt_types.localType(v_types[1]), nullptr);
		EXPECT_EQ(vt_types.slotType(v_types[1]), nullptr);
		EXPECT_NE(vt_types.localType(v_types[2]), nullptr);
		EXPECT_NE(vt_types.localType(v_types[3]), nullptr);
	}
}

TEST (BackendArrayDescriptor, NullableRowsUseCheckedStridesAndShareKnownShapeSuffixes) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const cloth::CheckedProgram  cp_program = descriptorProgram(u_bits, R"(typedef<struct> Grid { pub int[2]?[3]?[0]? rows }
fn shape(Grid grid) {}
pub fn<int> main() { Grid grid = {[null, [[], null, []]]}; Grid copy = grid; grid.rows = null; return 42 })");
		const backend::LoweredModule lm_module(cp_program);
		ASSERT_NO_THROW(lm_module.verify());
		backend::detail::ValueTypes vt_types(lm_module.get(), cp_program);
		const cloth::ResolvedType &rt_grid = cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shape").front()).parameters.front();
		const cloth::ResolvedType &rt_field = cp_program.structLayouts.at(rt_grid).fields.front().type;
		const cloth::ResolvedType  rt_outer = rt_field.element(), rt_row = rt_outer.element().element();
		const LLVMTypeRef          ty_outer = vt_types.get(rt_outer), ty_row = vt_types.get(rt_row);
		ASSERT_NE(ty_outer, nullptr);
		ASSERT_NE(ty_row, nullptr);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_outer), 7u);
		ASSERT_EQ(LLVMCountStructElementTypes(ty_row), 6u);
		const unsigned          u_word         = u_bits / 8;
		const LLVMTargetDataRef td_target      = LLVMGetModuleDataLayout(lm_module.get());
		const LLVMTypeRef       ty_outerBuffer = vt_types.slotType(rt_outer), ty_rowBuffer = vt_types.slotType(rt_row);
		EXPECT_EQ(LLVMABISizeOfType(td_target, LLVMGetElementType(ty_outerBuffer)), std::uint64_t{ 8 } * u_word);
		EXPECT_EQ(LLVMABISizeOfType(td_target, LLVMGetElementType(ty_rowBuffer)), std::uint64_t{ 5 } * u_word);
		EXPECT_EQ(LLVMStructGetTypeAtIndex(LLVMGetElementType(ty_outerBuffer), 1), ty_row);
		EXPECT_EQ(LLVMGetTypeKind(LLVMStructGetTypeAtIndex(LLVMGetElementType(vt_types.localType(rt_outer)), 1)), LLVMArrayTypeKind);
		const cloth::StorageLayout sl_outer{ .size = std::uint64_t{ 16 } * u_word, .alignment = u_word };
		const cloth::StorageLayout sl_row{ .size = std::uint64_t{ 15 } * u_word, .alignment = u_word };
		EXPECT_EQ(cp_program.arrayDataLayout(rt_outer, 2), sl_outer);
		EXPECT_EQ(cp_program.arrayDataLayout(rt_row, 3), sl_row);

		std::array<LLVMValueRef, 2> a_buffers{}, a_shapes{};
		LLVMValueRef                ir_suffix = nullptr, ir_witness = nullptr;
		unsigned                    u_roots = 0;
		const LLVMValueRef          fn_main = LLVMGetNamedFunction(lm_module.get(), "main");
		for (LLVMBasicBlockRef bb_block = LLVMGetFirstBasicBlock(fn_main); bb_block; bb_block = LLVMGetNextBasicBlock(bb_block)) {
			for (LLVMValueRef ir_inst = LLVMGetFirstInstruction(bb_block); ir_inst; ir_inst = LLVMGetNextInstruction(ir_inst)) {
				if (!LLVMIsAStoreInst(ir_inst)) continue;
				const LLVMValueRef ir_value = LLVMGetOperand(ir_inst, 0);
				if (LLVMTypeOf(ir_value) == vt_types.get(rt_field) && LLVMIsAConstantStruct(ir_value) && !ir_witness)
					ir_witness = LLVMGetAggregateElement(ir_value, 1);
				if (!LLVMIsAInsertValueInst(ir_value)) continue;
				const LLVMValueRef ir_header = LLVMGetOperand(ir_value, 0);
				if (LLVMTypeOf(ir_value) == ty_outer) {
					ASSERT_LT(u_roots, a_buffers.size());
					a_buffers[u_roots]  = LLVMGetOperand(ir_value, 1);
					a_shapes[u_roots++] = LLVMGetAggregateElement(ir_header, 4);
				} else if (LLVMTypeOf(ir_value) == ty_row) {
					ir_suffix = LLVMGetAggregateElement(ir_header, 3);
				}
			}
		}
		ASSERT_EQ(u_roots, 2u);
		EXPECT_NE(a_buffers[0], a_buffers[1]);
		EXPECT_EQ(a_shapes[0], a_shapes[1]);
		ASSERT_NE(a_shapes[0], nullptr);
		ASSERT_NE(LLVMIsAGlobalVariable(a_shapes[0]), nullptr);
		EXPECT_TRUE(LLVMIsGlobalConstant(a_shapes[0]));
		const LLVMValueRef            ir_bounds = LLVMGetInitializer(a_shapes[0]);
		const std::array<unsigned, 3> a_counts{ 2, 3, 0 };
		ASSERT_EQ(LLVMGetArrayLength2(LLVMTypeOf(ir_bounds)), a_counts.size());
		for (unsigned u_index = 0; const unsigned u_count : a_counts) {
			const LLVMValueRef ir_extent = LLVMGetAggregateElement(ir_bounds, u_index++);
			EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_extent, 0)), u_count);
			EXPECT_EQ(LLVMConstIntGetZExtValue(LLVMGetAggregateElement(ir_extent, 1)), 1u);
		}
		LLVMValueRef ir_one = LLVMConstInt(LLVMIntPtrTypeInContext(LLVMGetModuleContext(lm_module.get()), td_target), 1, false);
		EXPECT_EQ(ir_suffix, LLVMConstGEP2(LLVMGetElementType(LLVMTypeOf(ir_bounds)), a_shapes[0], &ir_one, 1));
		ASSERT_NE(ir_witness, nullptr);
		for (const unsigned u_index : { 0u, 4u, 5u, 6u }) EXPECT_TRUE(LLVMIsNull(LLVMGetAggregateElement(ir_witness, u_index)));
	}
}

TEST (BackendArrayDescriptor, NullableRowBackingMustFitEvenWhenSnapshotsAreSmall) {
	for (const unsigned u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const std::uint64_t         u64_chunk = std::uint64_t{ 1 } << (u_bits - 4);
		const std::string           s_count   = std::to_string(u64_chunk / (std::uint64_t{ 4 } * (u_bits / 8)));
		const cloth::CheckedProgram cp_program =
		        descriptorProgram(u_bits, "typedef<struct> Cell { int<8>[0] cells }\nfn shapes(Cell[16]?[" + s_count + "] tooLarge, Cell[0]?[" +
		                                          s_count + "] empty, Cell[16]?[0] emptyRows, Cell[2]?[] growableRow) {}\npub fn<int> main() = 42");
		const backend::LoweredModule lm_module(cp_program);
		backend::detail::ValueTypes  vt_types(lm_module.get(), cp_program);
		const cloth::ResultTypes    &v_types = cp_program.declarations.at(&cp_program.units.front()->getGlobals().at("shapes").front()).parameters;
		ASSERT_TRUE(cp_program.arrayDataLayout(v_types[0], 16).has_value());
		const LLVMTypeRef ty_row = vt_types.slotType(v_types[0].element().element());
		ASSERT_NE(ty_row, nullptr);
		EXPECT_EQ(LLVMABISizeOfType(LLVMGetModuleDataLayout(lm_module.get()), ty_row), u64_chunk);
		EXPECT_EQ(vt_types.localType(v_types[0]), nullptr);
		EXPECT_NE(vt_types.localType(v_types[1]), nullptr);
		EXPECT_NE(vt_types.localType(v_types[2]), nullptr);
		EXPECT_EQ(vt_types.localType(v_types[3]), nullptr);
	}
}
}
