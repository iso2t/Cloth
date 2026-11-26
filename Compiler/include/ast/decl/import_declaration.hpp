// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <token/token.hpp>

namespace cloth {

struct ImportPath {
	std::vector<Token> components;
	std::vector<Token> dots;

	[[nodiscard]] TokenSpan getSpan () const {
		const TokenSpan ts_first = components.at(0).getSpan();
		return TokenSpan::spanThrough(ts_first, components.back().getSpan());
	}

	[[nodiscard]] std::string getSpelling () const {
		std::string s_spelling;
		for (std::size_t st_index = 0; st_index < components.size(); ++st_index) {
			if (st_index != 0) {
				s_spelling += toString(ODot);
			}
			s_spelling += components[st_index].getLexeme();
		}
		return s_spelling;
	}
};

struct ImportSelection {
	struct Names {
		std::vector<Token> names;
		std::vector<Token> commas;

		[[nodiscard]] TokenSpan getSpan () const {
			const TokenSpan ts_first = names.at(0).getSpan();
			return TokenSpan::spanThrough(ts_first, names.back().getSpan());
		}
	};

	struct Wildcard {
		Token star;

		[[nodiscard]] TokenSpan getSpan () const {
			return star.getSpan();
		}
	};

	std::variant<Names, Wildcard> value;

	[[nodiscard]] TokenSpan getSpan () const {
		return std::visit([]<typename Selection>(const Selection &selection) -> TokenSpan { return selection.getSpan(); }, value);
	}
};

struct ImportDeclaration {
	Token           keyword;
	ImportPath      path;
	Token           colon;
	ImportSelection selection;
	Token           terminator;

	[[nodiscard]] TokenSpan getSpan () const {
		return TokenSpan::spanThrough(keyword.getSpan(), selection.getSpan());
	}
};

}
