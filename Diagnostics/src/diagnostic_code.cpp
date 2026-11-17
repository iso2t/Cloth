// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>

#include <diagnostic_code.hpp>

namespace cde {

std::string to_string (DiagnosticCode code) {
	return std::format("C{:04}", static_cast<std::uint16_t>(code));
}

}
