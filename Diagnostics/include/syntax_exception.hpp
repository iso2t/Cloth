// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <vector>

#include <token/token.hpp>

namespace cde {
class SyntaxException : public ClothDiagnostic {
    public:
	SyntaxException (DiagnosticCode code, std::string message, cloth::TokenSpan span, std::optional<std::string> help, std::vector<SourceLabel> related) :
	        d_code(code),
	        s_message(message),
	        t_span(span),
	        s_help(help),
	        v_related(related) {
	}

    private:
	DiagnosticCode             d_code;
	std::string                s_message;
	cloth::TokenSpan           t_span;
	std::optional<std::string> s_help    = std::nullopt;
	std::vector<SourceLabel>   v_related = {};
};
}
