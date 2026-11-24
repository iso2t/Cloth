// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <diagnostics.hpp>

namespace cde {

ClothDiagnostic::ClothDiagnostic (DiagnosticCode code, std::string message, std::vector<SourceLabel> labels, std::optional<std::string> guidance,
                                  Severity severity) :
        d_code(code),
        e_severity(severity),
        s_message(std::move(message)),
        v_labels(std::move(labels)),
        opt_guidance(std::move(guidance)) {
}

}
