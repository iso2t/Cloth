// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <cde/diagnostic_code.hpp>
#include <token/token.hpp>

namespace cde {

enum class Severity {
	Error,
	Warning,
	Information,
	Hint,
};

struct SourceLabel {
	cloth::TokenSpan span;
	std::string      text;
};

class ClothDiagnostic : public std::exception {
    public:
	// The first source label, when present, is primary.
	ClothDiagnostic (DiagnosticCode code, std::string message, std::vector<SourceLabel> labels = {}, std::optional<std::string> guidance = std::nullopt,
	                 Severity severity = Severity::Error);

	[[nodiscard]] const char *what () const noexcept override {
		return s_message.c_str();
	}

	[[nodiscard]] DiagnosticCode get_code () const noexcept {
		return d_code;
	}

	[[nodiscard]] Severity get_severity () const noexcept {
		return e_severity;
	}

	[[nodiscard]] const std::string &get_message () const noexcept {
		return s_message;
	}

	[[nodiscard]] const std::vector<SourceLabel> &get_source_labels () const noexcept {
		return v_labels;
	}

	[[nodiscard]] const std::optional<std::string> &get_guidance () const noexcept {
		return opt_guidance;
	}

    private:
	DiagnosticCode             d_code;
	Severity                   e_severity;
	std::string                s_message;
	std::vector<SourceLabel>   v_labels;
	std::optional<std::string> opt_guidance;
};

}
