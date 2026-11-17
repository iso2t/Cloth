// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <token/token.hpp>

namespace cloth {

struct ImportPath {
	std::vector<Token> components;
	std::vector<Token> dots;

	TokenSpan get_span () const {
		const TokenSpan ts_first = components.at(0).get_span();
		return TokenSpan::span_through(ts_first, components.back().get_span());
	}

	std::string get_spelling () const {
		std::string s_spelling;
		for (std::size_t st_index = 0; st_index < components.size(); ++st_index) {
			if (st_index != 0) {
				s_spelling += cloth::to_string(Operator::o_DOT);
			}
			s_spelling += components[st_index].get_lexeme();
		}
		return s_spelling;
	}
};

struct ImportSelection {
	struct Names {
		std::vector<Token> names;
		std::vector<Token> commas;

		TokenSpan get_span () const {
			const TokenSpan ts_first = names.at(0).get_span();
			return TokenSpan::span_through(ts_first, names.back().get_span());
		}
	};

	struct Wildcard {
		Token star;

		TokenSpan get_span () const {
			return star.get_span();
		}
	};

	std::variant<Names, Wildcard> value;

	TokenSpan get_span () const {
		return std::visit([]<typename Selection>(const Selection &selection) -> TokenSpan { return selection.get_span(); }, value);
	}
};

struct ImportDeclaration {
	Token                keyword;
	ImportPath           path;
	Token                colon;
	ImportSelection      selection;
	std::optional<Token> terminator = std::nullopt;

	TokenSpan get_span () const {
		return TokenSpan::span_through(keyword.get_span(), selection.get_span());
	}
};

}
