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

	TokenSpan get_span () const {
		return declaration.span;
	}
};

struct TypeParameterBound {
	std::variant<ConstructorBound, TypeReference> value;

	TokenSpan get_span () const {
		return std::visit([]<typename Bound>(const Bound &bound) -> TokenSpan { return bound.get_span(); }, value);
	}
};

struct TypeParameterDeclaration {
	Token                           name;
	std::vector<TypeParameterBound> bounds      = {};
	std::optional<TypeReference>    defaultType = std::nullopt;

	TokenSpan get_span () const {
		const TokenSpan ts_name = name.get_span();
		if (defaultType) {
			return TokenSpan::span_through(ts_name, defaultType->get_span());
		}
		if (!bounds.empty()) {
			return TokenSpan::span_through(ts_name, bounds.back().get_span());
		}
		return ts_name;
	}

	std::vector<TypeReference> get_type_bounds () const {
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

	TokenSpan get_span () const {
		return TokenSpan::span_through(opening.get_span(), closing.get_span());
	}
};

}
