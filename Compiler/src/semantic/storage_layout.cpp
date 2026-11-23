// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Integers.rst

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <utility>

#include <semantic/checker.hpp>

namespace cloth {
namespace {

std::uint64_t padding (std::uint64_t offset, unsigned alignment) {
	return (0 - offset) & (alignment - 1);
}

[[noreturn]] void structTooLarge (const ResolvedType &type, const Symbol &field, const TargetLayout &target) {
	semantic::fail(semantic::Code::INVALID_STRUCT_FIELD, field.name.get_span(),
	               std::format("Storage of {} '{}' exceeds {} on a {}-bit target", to_string(kStruct), type.spelling(), to_string(kUsize),
	                           target.sizeBits));
}

// A field starts at its ABI alignment. Tail padding is applied once all fields are placed.
class StructStorage {
    public:
	explicit StructStorage (const TargetLayout &target) : u64_limit(target.maximumSize()) {
	}

	std::optional<std::uint64_t> append (StorageLayout field) {
		const std::uint64_t u64_padding = padding(u64_end, field.alignment);
		if (u64_padding > u64_limit - u64_end) return {};
		const std::uint64_t u64_offset = u64_end + u64_padding;
		if (field.size > u64_limit - u64_offset) return {};
		u64_end     = u64_offset + field.size;
		u_alignment = std::max(u_alignment, field.alignment);
		return u64_offset;
	}

	[[nodiscard]] std::optional<StorageLayout> finish () const {
		const std::uint64_t u64_padding = padding(u64_end, u_alignment);
		if (u64_padding > u64_limit - u64_end) return {};
		return StorageLayout{ std::max<std::uint64_t>(1, u64_end + u64_padding), u_alignment };
	}

    private:
	std::uint64_t u64_limit;
	std::uint64_t u64_end     = 0;
	unsigned      u_alignment = 1;
};

}

StorageLayout TargetLayout::integerStorage (unsigned width) const {
	if (!width || width > MAX_INTEGER_BITS) throw std::invalid_argument("Unsupported integer storage width");
	const unsigned u_bytes = (width + 7) / 8;
	// The native integer ABI entries are i8, i16, i32, i64, and i128. Wider integers use the last entry.
	unsigned u_alignment = 16;
	if (width <= 32)
		u_alignment = std::bit_ceil(u_bytes);
	else if (width <= 64)
		u_alignment = sizeBits == 32 ? scalar64Alignment32 : 8;
	return { u_bytes + padding(u_bytes, u_alignment), u_alignment };
}

std::optional<StorageLayout> CheckedProgram::storageLayout (const ResolvedType &type) const {
	if (type.dependent()) return {};
	if (type.nullable()) {
		const std::optional<StorageLayout> opt_payload = storageLayout(type.element());
		if (!opt_payload || opt_payload->size > layout.maximumSize() - opt_payload->alignment) return {};
		// The presence byte and its padding precede the aligned payload.
		return StorageLayout{ opt_payload->alignment + opt_payload->size, opt_payload->alignment };
	}
	const StorageLayout sl_word = layout.integerStorage(layout.sizeBits);
	if (type.array()) {
		// Data pointer, length, capacity, and owner. Each inner extent has a count and a knownness flag.
		std::uint64_t u64_rank       = 0;
		bool          b_nullableRows = false;
		for (const std::string &s_suffix : type.suffixes) {
			if (s_suffix == to_string(oQuestion))
				b_nullableRows |= u64_rank != 0;
			else
				++u64_rank;
		}
		const std::uint64_t u64_words = 4 + 2 * (u64_rank - 1) + b_nullableRows;
		if (u64_words > layout.maximumSize() / sl_word.size) return {};
		return StorageLayout{ u64_words * sl_word.size, sl_word.alignment };
	}
	if (type.integer() || type.floating()) return layout.integerStorage(type.width ? type.width : layout.sizeBits);
	if (type == ResolvedType(kBoolean)) return layout.integerStorage(8);
	if (type == ResolvedType(kCharacter) || semantic::enumeration(type)) return layout.integerStorage(32);
	if (type == ResolvedType(kTypeinfo)) return sl_word;
	if (type == ResolvedType(kString)) return StorageLayout{ 2 * sl_word.size, sl_word.alignment };
	if (semantic::asClass(type.declaration) || semantic::asInterface(type.declaration)) return layout.integerStorage(64);
	if (const ResolvedType *rt_endpoint = semantic::rangeEndpoint(type)) {
		const std::optional<StorageLayout> opt_endpoint = storageLayout(*rt_endpoint);
		if (!opt_endpoint) return {};
		StructStorage ss_range(layout);
		if (!ss_range.append(*opt_endpoint) || !ss_range.append(*opt_endpoint) || !ss_range.append(layout.integerStorage(8))) return {};
		return ss_range.finish();
	}
	const std::map<ResolvedType, StructLayout>::const_iterator it_struct = structLayouts.find(type);
	if (it_struct == structLayouts.end()) return {};
	return it_struct->second.storage;
}

std::optional<StorageLayout> CheckedProgram::arrayDataLayout (const ResolvedType &array, std::uint64_t count) const {
	if (!array.array()) return {};
	const std::optional<StorageLayout> opt_element = storageLayout(array.element());
	if (!opt_element || count > layout.maximumSize() / opt_element->size) return {};
	return StorageLayout{ count * opt_element->size, opt_element->alignment };
}

namespace semantic {

void Analyzer::useArrayLiteral (const ResolvedType &type, const ArrayLiteral &literal) {
	v_arrayLiterals.push_back({ type, literal.elements.size(), literal.get_span() });
}

void Analyzer::layoutStruct (const ResolvedType &type, const StorageFields &fields) const {
	StructStorage              ss_fields(program.layout);
	StructLayout               sl_result{};
	std::vector<StorageLayout> v_shapeWitnesses;
	for (const StorageFields::value_type &field : fields) {
		const std::optional<StorageLayout> opt_storage = program.storageLayout(field.type);
		if (!opt_storage) structTooLarge(type, *field.symbol, program.layout);
		const std::optional<std::uint64_t> opt_offset = ss_fields.append(*opt_storage);
		if (!opt_offset) structTooLarge(type, *field.symbol, program.layout);
		sl_result.fieldOffsets.emplace(field.symbol, *opt_offset);
		// A nullable array field retains its shape after the value is cleared.
		if (field.type.nullable() && field.type.element().array()) v_shapeWitnesses.push_back(*opt_storage);
	}
	for (StorageLayout sl_witness : v_shapeWitnesses) {
		if (!ss_fields.append(sl_witness)) structTooLarge(type, *type.declaration, program.layout);
	}
	const std::optional<StorageLayout> opt_storage = ss_fields.finish();
	if (!opt_storage) structTooLarge(type, *type.declaration, program.layout);
	sl_result.storage = *opt_storage;
	sl_result.fields  = fields;
	program.structLayouts.emplace(type, std::move(sl_result));
}

}
}
