// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {
#define MAX_TYPE_ARG_NESTING 256

struct ConstructorBound {
	ConstructorGuaranteeDeclaration declaration;

	[[nodiscard]] TokenSpan getSpan () const {
		return declaration.span;
	}
};

struct TypeParameterBound {
	std::variant<ConstructorBound, TypeReference> value;

	[[nodiscard]] TokenSpan getSpan () const {
		return std::visit([]<typename Bound>(const Bound &bound) -> TokenSpan { return bound.getSpan(); }, value);
	}
};

enum class PrimitiveFamily { Integer, Float, String, Char, Bool };

constexpr bool numericFamily (PrimitiveFamily family) {
	return family == PrimitiveFamily::Integer || family == PrimitiveFamily::Float;
}

constexpr std::string_view primitiveFamilyName (PrimitiveFamily family) {
	switch (family) {
	case PrimitiveFamily::Integer:
		return "integer";
	case PrimitiveFamily::Float:
		return "float";
	case PrimitiveFamily::String:
		return "string";
	case PrimitiveFamily::Char:
		return "char";
	case PrimitiveFamily::Bool:
		return "bool";
	}
	return {};
}

constexpr std::optional<PrimitiveFamily> primitiveFamilyForBinding (std::string_view id) {
	constexpr std::string_view prefix = "primitive.";
	if (!id.starts_with(prefix)) return {};
	id.remove_prefix(prefix.size());
	for (const auto family : { PrimitiveFamily::Integer, PrimitiveFamily::Float, PrimitiveFamily::String, PrimitiveFamily::Char, PrimitiveFamily::Bool })
		if (id == primitiveFamilyName(family)) return family;
	return {};
}

struct TypeParameterDeclaration {
	Token                           name;
	std::vector<TypeParameterBound> bounds      = {};
	std::optional<TypeReference>    defaultType = std::nullopt;
	std::optional<PrimitiveFamily>  primitiveSelf = {};

	[[nodiscard]] TokenSpan getSpan () const {
		TokenSpan ts_name = name.getSpan();
		if (defaultType) {
			return TokenSpan::spanThrough(ts_name, defaultType->getSpan());
		}
		if (!bounds.empty()) {
			return TokenSpan::spanThrough(ts_name, bounds.back().getSpan());
		}
		return ts_name;
	}

	[[nodiscard]] std::vector<TypeReference> getTypeBounds () const {
		std::vector<TypeReference> v_typeBounds;
		for (const TypeParameterBound &tpb_bound : bounds) {
			if (const TypeReference *tr_reference = std::get_if<TypeReference>(&tpb_bound.value)) {
				v_typeBounds.push_back(*tr_reference);
			}
		}
		return v_typeBounds;
	}
};

struct TypeParameterList {
	Token                                 opening;
	std::vector<TypeParameterDeclaration> parameters;
	Token                                 closing;

	[[nodiscard]] TokenSpan getSpan () const {
		return TokenSpan::spanThrough(opening.getSpan(), closing.getSpan());
	}
};

}
