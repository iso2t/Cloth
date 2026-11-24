// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
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

struct TypeParameterDeclaration {
	Token                           name;
	std::vector<TypeParameterBound> bounds      = {};
	std::optional<TypeReference>    defaultType = std::nullopt;

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
