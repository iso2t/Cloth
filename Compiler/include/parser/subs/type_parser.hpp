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
	bool                             startsCallTypeArguments (int nameOffset = 0);
	bool                             startsNamedTypeReceiver ();
	bool                             startsQualifiedTypeCheck ();
	static bool                      startsTypeReceiver (const Token &token);

	template <typename ParseClause>
	std::vector<std::invoke_result_t<ParseClause &, TypeReference, const Token &>> parseParents (std::string_view description, ParseClause &&parseClause) {
		using Clause = std::invoke_result_t<ParseClause &, TypeReference, const Token &>;
		std::vector<Clause> v_parents;
		if (!at(Operator::o_COLON)) {
			return v_parents;
		}
		do {
			ParentType pt_parent = parseParent(description);
			v_parents.push_back(parseClause(std::move(pt_parent.type), std::as_const(pt_parent.separator)));
		} while (at(Operator::o_COMMA));
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
	std::optional<int>              angleListEnd (int start);
	std::optional<int>              applicationEnd (int start);
	std::optional<int>              suffixEnd (int start);
	int                             nameEnd ();
	bool                            typeCanEndAt (int offset);
	Token                           parseName ();
	std::optional<TokenSpan>        nullableMarker ();
	bool                            isWidthToken (const Token &token) const;
	bool                            at (Operator op, int offset = 0);
};

}
