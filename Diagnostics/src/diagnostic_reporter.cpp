// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

#include <cloth_diagnostic.hpp>
#include <diagnostic_reporter.hpp>
#include <file/source_file.hpp>

namespace cde {
namespace {

struct TerminalStyle {
	bool unicode;
	bool color;
};

TerminalStyle terminal_style (const std::ostream &output) {
	const char *s_term = std::getenv("TERM");
	if (&output != &std::cerr || (s_term && std::string_view(s_term) == "dumb")) {
		return {};
	}

	const char *s_noColor = std::getenv("NO_COLOR");
	const bool  b_color   = !s_noColor || !*s_noColor;
#ifdef _WIN32
	DWORD mode;
	if (!GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode)) {
		return {};
	}
	return { GetConsoleOutputCP() == CP_UTF8, b_color && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 };
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

ReportLevel report_level (Severity severity) {
	switch (severity) {
	case Severity::Warning:
		return { MU_WARNING, "Warning" };
	case Severity::Information:
		return { MU_CUSTOM_LEVEL, "Information" };
	case Severity::Hint:
		return { MU_CUSTOM_LEVEL, "Hint" };
	case Severity::Error:
		return { MU_ERROR, "Error" };
	}
	return { MU_ERROR, "Error" };
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
		const auto s_path = source.get_path().u8string();
		name.assign(s_path.begin(), s_path.end());

		const std::string_view input = source.get_contents();
		text.reserve(input.size());
		std::size_t start = 0;
		for (;;) {
			const auto newline = input.find('\n', start);
			auto       line    = input.substr(start, newline == std::string_view::npos ? newline : newline - start);
			const bool crlf    = newline != std::string_view::npos && line.ends_with('\r');
			if (crlf) {
				line.remove_suffix(1);
			}
			lines.push_back({ text.size(), line.size(), crlf });
			text.append(line);

			if (newline == std::string_view::npos) {
				break;
			}
			text.push_back('\n');
			start = newline + 1;
		}
	}

	std::optional<std::size_t> byte_offset (std::size_t line, std::size_t column) const {
		if (line == 0 || line > lines.size() || column == 0) {
			return std::nullopt;
		}
		const auto &source_line = lines[line - 1];
		const auto  byte        = column - 1;
		if (byte > source_line.length + source_line.crlf) {
			return std::nullopt;
		}
		// Positions on either byte of CRLF map to the display newline.
		const auto offset = source_line.offset + std::min(byte, source_line.length);
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

bool add_source (SourceCache &cache, std::string_view text, std::string_view name) {
	mu_Cache   *updated = cache.get();
	const auto *source  = mu_addmemory(&updated, slice(text), slice(name));
	if (updated && updated != cache.get()) {
		// A multi-source cache takes ownership of the previous single-source cache.
		cache.release();
		cache.reset(updated);
	}
	return source != nullptr;
}

int append_output (void *output, const char *text, std::size_t length) noexcept {
	try {
		static_cast<std::string *>(output)->append(text, length);
		return MU_OK;
	} catch (...) {
		return MU_ERRFILE;
	}
}

std::optional<std::string> render (const ClothDiagnostic &diagnostic, std::string_view code, ReportLevel level, TerminalStyle style) {
	std::unordered_map<const cloth::SourceFile *, DisplaySource> sources;
	std::string                                                  output;
	mu_Config                                                    config;
	SourceCache                                                  cache(nullptr, mu_delcache);
	std::unique_ptr<mu_Report, decltype(&mu_delete)>             report(mu_new(nullptr, nullptr), mu_delete);
	if (!report) {
		return std::nullopt;
	}

	mu_initconfig(&config);
	config.char_set   = style.unicode ? mu_unicode() : mu_ascii();
	config.color      = style.color ? mu_default_color : nullptr;
	config.index_type = MU_INDEX_BYTE;
	mu_config(report.get(), &config);
	mu_title(report.get(), level.level, slice(level.name), slice(diagnostic.get_message()));
	mu_code(report.get(), slice(code));

	bool first_label = true;
	for (const auto &label : diagnostic.get_source_labels()) {
		const auto &span = label.span;
		if (!span.source) {
			continue;
		}

		const auto [entry, inserted] = sources.try_emplace(span.source.get(), *span.source, static_cast<mu_Id>(sources.size()));
		const auto &source           = entry->second;
		if (inserted && !add_source(cache, source.text, source.name)) {
			return std::nullopt;
		}

		const auto start = source.byte_offset(span.startLine, span.startCol);
		const auto end   = source.byte_offset(span.endLine, span.endCol);
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
	if (!cache && !add_source(cache, "", "")) {
		return std::nullopt;
	}
	if (const auto &guidance = diagnostic.get_guidance()) {
		mu_help(report.get(), slice(*guidance));
	}
	mu_writer(report.get(), append_output, &output);
	if (mu_render(report.get(), cache.get()) != MU_OK) {
		return std::nullopt;
	}
	return output;
}

}

void DiagnosticReporter::report (const ClothDiagnostic &diagnostic) const {
	const auto code  = to_string(diagnostic.get_code());
	const auto level = report_level(diagnostic.get_severity());
	if (const auto output = render(diagnostic, code, level, terminal_style(os_output))) {
		os_output << *output;
		return;
	}

	os_output << '[' << code << "] " << level.name << ": " << diagnostic.get_message() << '\n';
	if (const auto &guidance = diagnostic.get_guidance()) {
		os_output << "Help: " << *guidance << '\n';
	}
}

}
