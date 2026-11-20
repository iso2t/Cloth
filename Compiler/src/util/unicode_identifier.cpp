// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utf8proc/utf8proc.h>

#include <token/token.hpp>
#include <util/unicode_identifier.hpp>
#include <util/utf8.hpp>

namespace cloth {
namespace {

struct IdentifierRange {
	char32_t first;
	char32_t last;
	bool     canStart;
};

constexpr IdentifierRange XID_RANGES[] = {
#include <unicode/xid_ranges.inc>
};

std::size_t identifierLength (std::string_view text, bool start) {
	if (text.empty()) return 0;
	const unsigned char c_first = static_cast<unsigned char>(text.front());
	if (c_first < 0x80) {
		return (c_first >= 'a' && c_first <= 'z') || (c_first >= 'A' && c_first <= 'Z') || c_first == to_string(oUnderscore).front() ||
		       (!start && c_first >= '0' && c_first <= '9');
	}
	const std::size_t st_size = utf8_scalar_length(text);
	if (st_size == 0) return 0;
	const char32_t         c32_scalar = utf8_scalar_value(text.substr(0, st_size));
	const IdentifierRange *ir_range   = std::ranges::lower_bound(XID_RANGES, c32_scalar, {}, &IdentifierRange::last);
	return ir_range != std::end(XID_RANGES) && ir_range->first <= c32_scalar && (!start || ir_range->canStart) ? st_size : 0;
}

}

std::size_t identifierStartLength (std::string_view text) {
	return identifierLength(text, true);
}

std::size_t identifierContinueLength (std::string_view text) {
	return identifierLength(text, false);
}

std::string normalizeIdentifier (std::string_view spelling) {
	if (std::ranges::all_of(spelling, [] (unsigned char byte) { return byte < 0x80; })) return std::string(spelling);
	utf8proc_uint8_t      *p_output = nullptr;
	const utf8proc_ssize_t n_length =
	        utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(spelling.data()), spelling.size(), &p_output, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> p_owned(p_output, &std::free);
	if (n_length < 0) throw std::runtime_error(utf8proc_errmsg(n_length));
	return { reinterpret_cast<const char *>(p_owned.get()), static_cast<std::size_t>(n_length) };
}

}
