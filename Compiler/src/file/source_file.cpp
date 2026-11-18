// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <file/source_file.hpp>
#include <util/utf8.hpp>

namespace cloth {

SourceFile::SourceFile (std::filesystem::path path) :
        p_path(std::move(path)),
        s_name(source_name(p_path)),
        e_extension(source_extension(p_path)),
        s_contents(read_source(p_path)) {
}

SourceFile::SourceFile (std::filesystem::path path, std::string contents, Extension extension) :
        p_path(std::move(path)),
        s_name(source_name(p_path)),
        e_extension(extension),
        s_contents(std::move(contents)) {
}

/**
 * The two supported file types are dictated here.
 * All other file types are rejected.
 */
std::string_view to_string (Extension type) {
	switch (type) {
	case CLOTH_OBJECT:
		return "co";
	case LIBRARY:
		return "clib";
	default:
		return "UNKNOWN";
	}
}

std::string SourceFile::source_name (const std::filesystem::path &path) {
	if (const std::string s_name = path.stem().string(); !s_name.empty() || s_name != SEPERATOR) {
		return s_name;
	}
	throw std::invalid_argument("Invalid source file name '" + path.string() + "'");
}

Extension SourceFile::source_extension (const std::filesystem::path &path) {
	std::string s_suffix = path.extension().string();
	if (!s_suffix.empty()) {
		for (char &c : s_suffix) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}

		const std::string_view s_spelling = std::string_view{ s_suffix }.substr(1);
		for (const auto extension : { CLOTH_OBJECT, LIBRARY }) {
			if (s_spelling == to_string(extension)) {
				return extension;
			}
		}
	}

	throw std::invalid_argument("Unsupported extension for source file '" + path.string() + "' (expected .co or .clib)");
}

std::string SourceFile::read_source (const std::filesystem::path &path) {
	if (!std::filesystem::is_regular_file(path)) {
		throw std::runtime_error("Could not open source file '" + path.string() + "': expected a regular file");
	}

	std::ifstream ifs_input(path, std::ios::binary);
	if (!ifs_input) {
		throw std::runtime_error("Could not open source file '" + path.string() + "'");
	}

	std::string s_contents;
	char        c_buffer[4096];
	while (ifs_input.read(c_buffer, sizeof(c_buffer))) {
		s_contents.append(c_buffer, sizeof(c_buffer));
	}
	if (ifs_input.bad() || !ifs_input.eof()) {
		throw std::runtime_error("Could not read source file '" + path.string() + "'");
	}
	s_contents.append(c_buffer, static_cast<std::size_t>(ifs_input.gcount()));

	for (auto s_bytes = std::string_view{ s_contents }; !s_bytes.empty();) {
		const size_t st_length = utf8_scalar_length(s_bytes);
		if (st_length == 0) {
			const auto offset = s_contents.size() - s_bytes.size() + 1;
			throw std::runtime_error("Invalid UTF-8 in source file '" + path.string() + "' at byte " + std::to_string(offset));
		}
		s_bytes.remove_prefix(st_length);
	}
	return s_contents;
}

}
