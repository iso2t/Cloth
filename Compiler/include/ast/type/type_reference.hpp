// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <token/token.hpp>

namespace cloth {

class TypeReference;

struct TypeArgumentList {
	Token                      opening;
	std::vector<TypeReference> arguments;
	Token                      closing;

	[[nodiscard]] TokenSpan get_span () const {
		return TokenSpan::span_through(opening.get_span(), closing.get_span());
	}

	[[nodiscard]] bool hasPlaceholders () const;
};

class TypeReference {
    public:
	explicit TypeReference (const Token &name, bool isUnsigned = false, std::optional<Token> width = std::nullopt,
	                        std::optional<TokenSpan> span = std::nullopt, std::vector<TokenSpan> arraySuffixes = {},
	                        std::vector<std::optional<Token>> arraySizes = {}, std::optional<TokenSpan> nullable = std::nullopt,
	                        std::vector<std::optional<TokenSpan>> arrayNullability = {}, std::optional<TypeArgumentList> application = std::nullopt);

	[[nodiscard]] const Token &get_name () const {
		return t_name;
	}

	[[nodiscard]] bool isUnsigned () const {
		return b_unsigned;
	}

	[[nodiscard]] const std::optional<Token> &get_width () const {
		return opt_width;
	}

	[[nodiscard]] TokenSpan get_span () const {
		return t_span;
	}

	[[nodiscard]] const std::vector<TokenSpan> &get_array_suffixes () const {
		return v_arraySuffixes;
	}

	[[nodiscard]] const std::vector<std::optional<Token>> &get_array_sizes () const {
		return vopt_arraySizes;
	}

	[[nodiscard]] const std::optional<TokenSpan> &get_nullable () const {
		return opt_nullable;
	}

	[[nodiscard]] const std::vector<std::optional<TokenSpan>> &get_array_nullability () const {
		return vopt_arrayNullability;
	}

	[[nodiscard]] const std::optional<TypeArgumentList> &get_application () const {
		return opt_application;
	}

	[[nodiscard]] bool                              hasNullability () const;
	[[nodiscard]] bool                              is_placeholder () const;
	[[nodiscard]] bool                              capturesWidth () const;
	[[nodiscard]] const std::vector<TypeReference> &get_type_arguments () const;
	[[nodiscard]] const Token                      *get_bare_identifier () const;
	[[nodiscard]] std::string                       get_spelling () const;

    private:
	Token                                 t_name;
	bool                                  b_unsigned;
	std::optional<Token>                  opt_width;
	TokenSpan                             t_span;
	std::vector<TokenSpan>                v_arraySuffixes;
	std::vector<std::optional<Token>>     vopt_arraySizes;
	std::optional<TokenSpan>              opt_nullable;
	std::vector<std::optional<TokenSpan>> vopt_arrayNullability;
	std::optional<TypeArgumentList>       opt_application;
};

}
