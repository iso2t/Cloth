// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cde/cloth_diagnostic.hpp"
#include "cde/diagnostic_code.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include "checked_program_test.hpp"
#include "gtest/gtest.h"
#include "semantic/checked_program.hpp"
#include "semantic/symbol_table.hpp"
#include "semantic/target_layout.hpp"
#include "token/numeric_literals.hpp"

namespace cloth::test {
namespace {

TEST (SemanticStorage, IntegerAllocationSizeAndAlignmentFollowTheTarget) {
	for (unsigned const u_bits : { 32u, 64u }) {
		for (unsigned const u_align64 : { 4u, 8u }) {
			const TargetLayout tl_target{ .sizeBits=u_bits, .scalar64Alignment32=u_align64 };
			SCOPED_TRACE(::testing::Message() << u_bits << "-bit target, i64 alignment on 32-bit " << u_align64);
			EXPECT_EQ(tl_target.integerStorage(1), (StorageLayout{ 1, 1 }));
			EXPECT_EQ(tl_target.integerStorage(8), (StorageLayout{ 1, 1 }));
			EXPECT_EQ(tl_target.integerStorage(9), (StorageLayout{ 2, 2 }));
			EXPECT_EQ(tl_target.integerStorage(12), (StorageLayout{ 2, 2 }));
			EXPECT_EQ(tl_target.integerStorage(24), (StorageLayout{ 4, 4 }));
			EXPECT_EQ(tl_target.integerStorage(33), (StorageLayout{ 8, u_bits == 32 ? u_align64 : 8u }));
			EXPECT_EQ(tl_target.integerStorage(64), (StorageLayout{ 8, u_bits == 32 ? u_align64 : 8u }));
			EXPECT_EQ(tl_target.integerStorage(65), (StorageLayout{ 16, 16 }));
			EXPECT_EQ(tl_target.integerStorage(128), (StorageLayout{ 16, 16 }));
			EXPECT_EQ(tl_target.integerStorage(129), (StorageLayout{ 32, 16 }));
			EXPECT_EQ(tl_target.integerStorage(257), (StorageLayout{ 48, 16 }));
			EXPECT_EQ(tl_target.integerStorage(MAX_INTEGER_BITS), (StorageLayout{ 8192, 16 }));
		}
	}
}

TEST (SemanticStorage, FoldsScalarBytesAndKeepsArrayElementCounts) {
	for (unsigned const u_bits : { 32u, 64u }) {
		const CheckedProgram                    cp_program = checkSource(R"(fn<usize> signedBytes() = int<12>::size
fn<usize> unsignedBytes(unsigned int<12> sample) = sample::size
fn<usize> wideBytes() = unsigned int<257>::size
fn<usize> wordBytes() = isize::size
fn<usize> extent() = int<12>[3]::size
fn<usize> length(int<12>[] samples) = samples::size)",
		                                                                 { .sizeBits=u_bits });
		const SymbolScope                      &ss_globals = cp_program.units.front()->getGlobals();
		const std::pair<const char *, unsigned> v_counts[] = {
			{ "signedBytes", 2 }, { "unsignedBytes", 2 }, { "wideBytes", 48 }, { "wordBytes", u_bits / 8 }, { "extent", 3 }
		};
		for (const std::pair<const char *, unsigned> &entry : v_counts) {
			const ExpressionInfo &ei_size = cp_program.expressions.at(&bodyExpression(ss_globals.at(entry.first).front()));
			EXPECT_EQ(ei_size.numericConstant, NumericConstant(IntegerConstant{ { entry.second } })) << entry.first;
		}
		const ExpressionInfo &ei_length = cp_program.expressions.at(&bodyExpression(ss_globals.at("length").front()));
		EXPECT_EQ(ei_length.provider->kind, ProviderKind::ArraySize);
		EXPECT_TRUE(std::holds_alternative<std::monostate>(ei_length.numericConstant));
	}
}

TEST (SemanticStorage, PlacesFieldsAndStridesArraysOfConcreteTypes) {
	for (unsigned const u_bits : { 32u, 64u }) {
		SCOPED_TRACE(u_bits);
		const CheckedProgram cp_program = checkSource(R"(typedef<struct> Sample { int<8> z, int<12> a, int<8> m }
typedef<struct> Box<T> { int<8> tag, T value }
typedef<struct> Buffer { int<8> z, unsigned int<12>[3] samples, int<12> a }
fn layout(Sample[3] records, int<12>[] data, unsigned int<12>[3] fixedSamples, Box<int<12>> small,
          Box<int<24>> wide, Box<Sample> nested, Buffer buffer) {})",
		                                              { .sizeBits=u_bits });
		const SymbolScope   &ss_globals = cp_program.units.front()->getGlobals();
		const ResultTypes   &v_types    = cp_program.declarations.at(&ss_globals.at("layout").front()).parameters;
		const Symbol        &sym_sample = ss_globals.at("Sample").front();
		const StructLayout  &sl_sample  = cp_program.structLayouts.at(v_types[0].element());
		ASSERT_EQ(sl_sample.fields.size(), 3u);
		EXPECT_EQ(sl_sample.fields[0].symbol, &sym_sample.members.at("z").front());
		EXPECT_EQ(sl_sample.fields[1].symbol, &sym_sample.members.at("a").front());
		EXPECT_EQ(sl_sample.fields[2].symbol, &sym_sample.members.at("m").front());
		EXPECT_EQ(sl_sample.fieldOffsets.at(&sym_sample.members.at("z").front()), 0u);
		EXPECT_EQ(sl_sample.fieldOffsets.at(&sym_sample.members.at("a").front()), 2u);
		EXPECT_EQ(sl_sample.fieldOffsets.at(&sym_sample.members.at("m").front()), 4u);
		EXPECT_EQ(sl_sample.storage, (StorageLayout{ 6, 2 }));
		EXPECT_EQ(cp_program.arrayDataLayout(v_types[0], 3), (StorageLayout{ 18, 2 }));
		for (std::size_t const st_index : { 1u, 2u }) {
			EXPECT_EQ(cp_program.arrayDataLayout(v_types[st_index], 0), (StorageLayout{ 0, 2 }));
			EXPECT_EQ(cp_program.arrayDataLayout(v_types[st_index], 1), (StorageLayout{ 2, 2 }));
			EXPECT_EQ(cp_program.arrayDataLayout(v_types[st_index], 3), (StorageLayout{ 6, 2 }));
			const std::uint64_t u64_last = cp_program.layout.maximumSize() / 2;
			EXPECT_EQ(cp_program.arrayDataLayout(v_types[st_index], u64_last), (StorageLayout{ u64_last * 2, 2 }));
			EXPECT_FALSE(cp_program.arrayDataLayout(v_types[st_index], u64_last + 1));
		}
		EXPECT_EQ(cp_program.storageLayout(v_types[3]), (StorageLayout{ 4, 2 }));
		EXPECT_EQ(cp_program.storageLayout(v_types[4]), (StorageLayout{ 8, 4 }));
		EXPECT_EQ(cp_program.storageLayout(v_types[5]), (StorageLayout{ 8, 2 }));
		const Symbol &sym_value = ss_globals.at("Box").front().members.at("value").front();
		EXPECT_EQ(cp_program.structLayouts.at(v_types[3]).fields.at(1).type.width, 12u);
		EXPECT_EQ(cp_program.structLayouts.at(v_types[4]).fields.at(1).type.width, 24u);
		EXPECT_EQ(cp_program.structLayouts.at(v_types[3]).fieldOffsets.at(&sym_value), 2u);
		EXPECT_EQ(cp_program.structLayouts.at(v_types[4]).fieldOffsets.at(&sym_value), 4u);
		const std::uint64_t u64_word   = u_bits / 8;
		const Symbol       &sym_buffer = ss_globals.at("Buffer").front();
		const StructLayout &sl_buffer  = cp_program.structLayouts.at(v_types[6]);
		EXPECT_EQ(sl_buffer.fieldOffsets.at(&sym_buffer.members.at("samples").front()), u64_word);
		EXPECT_EQ(sl_buffer.fieldOffsets.at(&sym_buffer.members.at("a").front()), 5 * u64_word);
		EXPECT_EQ(sl_buffer.storage, (StorageLayout{ 6 * u64_word, u_bits / 8 }));
	}
}

TEST (SemanticStorage, UsesTheFieldAlignmentForWideArrayElements) {
	for (unsigned const u_bits : { 32u, 64u }) {
		const TargetLayout   tl_target{ .sizeBits=u_bits };
		const CheckedProgram cp_program = checkSource(R"(typedef<struct> Measure { int<8> first, int<48> value, int<8> last }
typedef<struct> Wide { int<8> first, unsigned int<257> value, int<8> last }
typedef<struct> OptionalSample { int<8> tag, int<12>? reading }
fn store(Measure[] measures, Wide[] blocks, int<257>[] scalars, int<12>?[] optionalSamples, OptionalSample tagged) {})",
		                                              tl_target);
		const SymbolScope   &ss_globals = cp_program.units.front()->getGlobals();
		const ResultTypes   &v_types    = cp_program.declarations.at(&ss_globals.at("store").front()).parameters;
		const unsigned       u_align    = tl_target.integerStorage(48).alignment;
		const StructLayout  &sl_measure = cp_program.structLayouts.at(v_types[0].element());
		EXPECT_EQ(sl_measure.fieldOffsets.at(&ss_globals.at("Measure").front().members.at("value").front()), u_align);
		EXPECT_EQ(cp_program.arrayDataLayout(v_types[0], 2), (StorageLayout{ 2 * (8 + 2 * u_align), u_align }));
		const StructLayout &sl_wide = cp_program.structLayouts.at(v_types[1].element());
		EXPECT_EQ(sl_wide.fieldOffsets.at(&ss_globals.at("Wide").front().members.at("value").front()), 16u);
		EXPECT_EQ(sl_wide.fieldOffsets.at(&ss_globals.at("Wide").front().members.at("last").front()), 64u);
		EXPECT_EQ(cp_program.arrayDataLayout(v_types[1], 2), (StorageLayout{ 160, 16 }));
		EXPECT_EQ(cp_program.arrayDataLayout(v_types[2], 2), (StorageLayout{ 96, 16 }));
		EXPECT_EQ(cp_program.arrayDataLayout(v_types[3], 3), (StorageLayout{ 12, 2 }));
		const StructLayout &sl_optional = cp_program.structLayouts.at(v_types[4]);
		EXPECT_EQ(sl_optional.fieldOffsets.at(&ss_globals.at("OptionalSample").front().members.at("reading").front()), 2u);
		EXPECT_EQ(sl_optional.storage, (StorageLayout{ 6, 2 }));
	}
}

TEST (SemanticStorage, DiagnosesUnrepresentableStructsAndArrayLiterals) {
	// Each level doubles its inline storage. The source never allocates the backing bytes.
	std::string s_source = "typedef<struct> Level0 { int<12> sample }\n";
	for (unsigned u_level = 1; u_level <= 30; ++u_level) {
		const std::string s_field = "Level" + std::to_string(u_level - 1);
		s_source += "typedef<struct> Level" + std::to_string(u_level) + " { " + s_field + " left, " + s_field + " right }\n";
	}
	const std::pair<std::string, cde::DiagnosticCode> v_overflows[] = {
		{ "fn pair(Level30 value) { def items = [value, value] }", cde::DiagnosticCode::InvalidArray },
		{ "typedef<struct> Pair { Level30 left, Level30 right }", cde::DiagnosticCode::InvalidStructField }
	};
	for (const std::pair<std::string, cde::DiagnosticCode> &entry : v_overflows) {
		EXPECT_NO_THROW(checkSource(s_source + entry.first, { 64 }));
		try {
			checkSource(s_source + entry.first, { .sizeBits=32 });
			FAIL() << "Expected target size overflow";
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.getCode(), entry.second) << error.what();
		}
	}
}

}
}
