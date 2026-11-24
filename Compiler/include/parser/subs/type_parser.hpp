// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class TypeParser {
    public:
	explicit TypeParser (Parser &parser) : p_parser(parser) {
	}

	TypeReference                    parse (bool inCast = false, bool allowArrays = false);
	std::optional<TypeParameterList> parseTypeParameters ();
	TypeArgumentList                 arguments ();
	bool                             startsCallTypeArguments (int nameOffset = 0) const;
	bool                             startsNamedTypeReceiver () const;
	bool                             startsQualifiedTypeCheck () const;
	std::optional<int>               typedTargetEnd (int start = 0) const;
	static bool                      startsTypeReceiver (const Token &token);

	template <typename ParseClause>
	std::vector<std::invoke_result_t<ParseClause &, TypeReference, const Token &>> parseParents (std::string_view description, ParseClause &&parseClause) {
		using Clause = std::invoke_result_t<ParseClause &, TypeReference, const Token &>;
		std::vector<Clause> v_parents;
		if (!at(OColon)) {
			return v_parents;
		}
		do {
			auto [separator, type] = parseParent(description);
			v_parents.push_back(parseClause(std::move(type), std::as_const(separator)));
		} while (at(OComma));
		return v_parents;
	}

    private:
	struct ParentType {
		Token         separator;
		TypeReference type;
	};

	Parser &p_parser;
	int     i_argumentDepth = 0;

	ParentType                      parseParent (std::string_view description);
	std::vector<TypeParameterBound> parseBounds ();
	std::optional<int>              angleListEnd (int start) const;
	std::optional<int>              applicationEnd (int start) const;
	std::optional<int>              suffixEnd (int start) const;
	int                             nameEnd (int start = 0) const;
	bool                            typeCanEndAt (int offset) const;
	Token                           parseName () const;
	std::optional<TokenSpan>        nullableMarker () const;
	[[nodiscard]] bool              isWidthToken (const Token &token) const;
	bool                            at (Operator op, int offset = 0) const;
};

}
