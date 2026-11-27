// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Diagnostics.rst
// See Diagnostics/README.rst

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <langinfo.h>
#include <locale.h>
#include <unistd.h>
#endif

#include <musubi.h>

#include <cde/cloth_diagnostic.hpp>
#include <cde/diagnostic_code.hpp>
#include <cde/diagnostic_reporter.hpp>
#include <file/source_file.hpp>

namespace cde {
namespace {

struct TerminalStyle {
	bool unicode;
	bool color;
};

TerminalStyle terminalStyle (const std::ostream &output) {
	if (const char *s_term = std::getenv("TERM"); &output != &std::cerr || (s_term && std::string_view(s_term) == "dumb")) {
		return {};
	}

	const char *s_noColor = std::getenv("NO_COLOR");
	const bool  b_color   = !s_noColor || !*s_noColor;
#ifdef _WIN32
	// MinGW's console headers require the types supplied by <windows.h>.
	// NOLINTBEGIN(misc-include-cleaner)
	DWORD mode = 0;
	if (!GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode)) {
		return {};
	}
	return { .unicode = GetConsoleOutputCP() == CP_UTF8, .color = b_color && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 };
	// NOLINTEND(misc-include-cleaner)
#else
	if (!isatty(STDERR_FILENO)) {
		return {};
	}
	locale_t   locale    = newlocale(LC_CTYPE_MASK, "", nullptr);
	const bool b_unicode = locale && std::string_view(nl_langinfo_l(CODESET, locale)) == "UTF-8";
	if (locale) {
		freelocale(locale);
	}
	return { b_unicode, b_color };
#endif
}

struct ReportLevel {
	mu_Level         level;
	std::string_view name;
};

ReportLevel reportLevel (Severity severity) {
	switch (severity) {
	case Severity::Warning:
		return { .level=MU_WARNING, .name="Warning" };
	case Severity::Information:
		return { .level=MU_CUSTOM_LEVEL, .name="Information" };
	case Severity::Hint:
		return { .level=MU_CUSTOM_LEVEL, .name="Hint" };
	case Severity::Error:
		return { .level=MU_ERROR, .name="Error" };
	}
	return { .level=MU_ERROR, .name="Error" };
}

struct SourceLine {
	std::size_t offset;
	std::size_t length;
	bool        crlf;
};

struct DisplaySource {
	std::string             name;
	std::string             text;
	std::vector<SourceLine> lines;
	mu_Id                   id;

	DisplaySource (const cloth::SourceFile &source, mu_Id id) : id(id) {
		const auto s_path = source.getPath().u8string();
		name.assign(s_path.begin(), s_path.end());

		const std::string_view input = source.getContents();
		text.reserve(input.size());
		std::size_t start = 0;
		for (;;) {
			const auto newline = input.find_first_of("\r\n", start);
			const auto line    = input.substr(start, newline == std::string_view::npos ? newline : newline - start);
			const bool crlf    = newline != std::string_view::npos && input.substr(newline).starts_with("\r\n");
			lines.push_back({ .offset=text.size(), .length=line.size(), .crlf=crlf });
			text.append(line);

			if (newline == std::string_view::npos) {
				break;
			}
			text.push_back('\n');
			start = newline + (crlf ? 2 : 1);
		}
	}

	[[nodiscard]] std::optional<std::size_t> byteOffset (const std::size_t line, const std::size_t column) const {
		if (line == 0 || line > lines.size() || column == 0) {
			return std::nullopt;
		}
		const auto &[oft, length, crlf] = lines[line - 1];
		const auto  byte        = column - 1;
		if (byte > length + crlf) {
			return std::nullopt;
		}
		// Positions on either byte of CRLF map to the display newline.
		const auto offset = oft + std::min(byte, length);
		if (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
			return std::nullopt;
		}
		return offset;
	}
};

mu_Slice slice (std::string_view text) {
	return mu_lslice(text.data(), text.size());
}

using SourceCache = std::unique_ptr<mu_Cache, decltype(&mu_delcache)>;

bool addSource (SourceCache &cache, std::string_view text, std::string_view name) {
	mu_Cache   *updated = cache.get();
	const auto *source  = mu_addmemory(&updated, slice(text), slice(name));
	if (updated && updated != cache.get()) {
		// A multi-source cache takes ownership of the previous single-source cache.
		cache.release();
		cache.reset(updated);
	}
	return source != nullptr;
}

int appendOutput (void *output, const char *text, std::size_t length) noexcept {
	try {
		static_cast<std::string *>(output)->append(text, length);
		return MU_OK;
	} catch (...) {
		return MU_ERRFILE;
	}
}

std::optional<std::string> render (const ClothDiagnostic &diagnostic, const std::string_view code, const ReportLevel &level, const TerminalStyle style) {
	std::unordered_map<const cloth::SourceFile *, DisplaySource> sources;
	std::string                                                  output;
	mu_Config                                                    config;
	SourceCache                                                  cache(nullptr, mu_delcache);
	std::unique_ptr<mu_Report, decltype(&mu_delete)>             const report(mu_new(nullptr, nullptr), mu_delete);
	if (!report) {
		return std::nullopt;
	}

	mu_initconfig(&config);
	config.char_set   = style.unicode ? mu_unicode() : mu_ascii();
	config.color      = style.color ? mu_default_color : nullptr;
	config.index_type = MU_INDEX_BYTE;
	mu_config(report.get(), &config);
	mu_title(report.get(), level.level, slice(level.name), slice(diagnostic.getMessage()));
	mu_code(report.get(), slice(code));

	bool first_label = true;
	for (const auto &label : diagnostic.getSourceLabels()) {
		const auto &span = label.span;
		if (!span.source) {
			continue;
		}

		const auto [entry, inserted] = sources.try_emplace(span.source.get(), *span.source, static_cast<mu_Id>(sources.size()));
		const auto &source           = entry->second;
		if (inserted && !addSource(cache, source.text, source.name)) {
			return std::nullopt;
		}

		const auto start = source.byteOffset(span.startLine, span.startCol);
		const auto end   = source.byteOffset(span.endLine, span.endCol);
		if (!start || !end || *start > *end) {
			continue;
		}
		mu_label(report.get(), *start, *end, source.id);
		mu_message(report.get(), slice(label.text), 0);
		if (first_label) {
			mu_primary(report.get());
			first_label = false;
		}
	}

	// Musubi requires an initialized cache, including for reports without labels.
	if (!cache && !addSource(cache, "", "")) {
		return std::nullopt;
	}
	if (const auto &guidance = diagnostic.getGuidance()) {
		mu_help(report.get(), slice(*guidance));
	}
	mu_writer(report.get(), appendOutput, &output);
	if (mu_render(report.get(), cache.get()) != MU_OK) {
		return std::nullopt;
	}
	return output;
}

}

void DiagnosticReporter::report (const ClothDiagnostic &diagnostic) const {
	const auto code  = toString(diagnostic.getCode());
	const auto level = reportLevel(diagnostic.getSeverity());
	if (const auto output = render(diagnostic, code, level, terminalStyle(os_output))) {
		os_output << *output;
		return;
	}

	os_output << '[' << code << "] " << level.name << ": " << diagnostic.getMessage() << '\n';
	if (const auto &guidance = diagnostic.getGuidance()) {
		os_output << "Help: " << *guidance << '\n';
	}
}

}
