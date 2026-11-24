// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdint>
#include <format>
#include <string>

#include <diagnostics.hpp>

namespace cde {

std::string toString (DiagnosticCode code) {
	return std::format("C{:04}", static_cast<std::uint16_t>(code));
}

}
