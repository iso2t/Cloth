// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <type_traits>
#include <utility>
#include <vector>

#include <token/token.hpp>

namespace cloth {

class Parser;

/** Parses the surrounding delimiters for a caller-supplied grammar. */
class DelimitedParser {
    public:
	explicit DelimitedParser (Parser &parser) : p_parser(parser) {
	}

	template <typename ParseContents> std::invoke_result_t<ParseContents> parse (const Operator open, const Operator close, ParseContents &&parseContents) {
		using ParsedContents = std::invoke_result_t<ParseContents>;
		expect(open);
		if constexpr (std::is_void_v<ParsedContents>) {
			std::forward<ParseContents>(parseContents)();
			expect(close);
		} else {
			ParsedContents pc_contents = std::forward<ParseContents>(parseContents)();
			expect(close);
			return pc_contents;
		}
	}

	template <typename ParseContents> std::invoke_result_t<ParseContents> angles (ParseContents &&parseContents) {
		return parse(oLess, oGreater, std::forward<ParseContents>(parseContents));
	}

	template <typename ParseContents> std::invoke_result_t<ParseContents> parentheses (ParseContents &&parseContents) {
		return parse(oParenOpen, oParenClose, std::forward<ParseContents>(parseContents));
	}

	template <typename ParseContents> std::invoke_result_t<ParseContents> brackets (ParseContents &&parseContents) {
		return parse(oBracketOpen, oBracketClose, std::forward<ParseContents>(parseContents));
	}

	template <typename ParseContents> std::invoke_result_t<ParseContents> braces (ParseContents &&parseContents) {
		return parse(oBraceOpen, oBraceClose, std::forward<ParseContents>(parseContents));
	}

	/** Parses zero or more items and leaves the closing token for the caller. */
	template <typename ParseItem> std::vector<std::invoke_result_t<ParseItem &>> commaSeparated (Operator close, ParseItem &&parseItem) {
		return commaSeparated(close, [] () -> bool { return false; }, std::forward<ParseItem>(parseItem));
	}

	/** Allows an early stop after a complete item. Each comma requires another item. */
	template <typename EndAfterItem, typename ParseItem>
	std::vector<std::invoke_result_t<ParseItem &>> commaSeparated (Operator close, EndAfterItem &&endAfterItem, ParseItem &&parseItem) {
		std::vector<std::invoke_result_t<ParseItem &>> v_items;
		skipNewLines();
		if (at(close)) {
			return v_items;
		}

		while (true) {
			v_items.push_back(parseItem());
			skipNewLines();
			if (at(close) || endAfterItem()) {
				break;
			}
			expect(oComma);
			skipNewLines();
		}
		return v_items;
	}

    private:
	Parser &p_parser;

	void expect (Operator op);
	bool at (Operator op);
	void skipNewLines ();
};

}
