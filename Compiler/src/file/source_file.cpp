// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <file/source_file.hpp>
#include <util/utf8.hpp>

namespace cloth {

std::string pathUtf8 (const std::filesystem::path &path) {
	const std::u8string s_utf8 = path.u8string();
	return { s_utf8.begin(), s_utf8.end() };
}

SourceFile::SourceFile (std::filesystem::path path) :
        p_path(std::move(path)),
        s_name(sourceName(p_path)),
        e_extension(sourceExtension(p_path)),
        s_contents(readSource(p_path)) {
}

SourceFile::SourceFile (std::filesystem::path path, std::string contents, Extension extension) :
        p_path(std::move(path)),
        s_name(sourceName(p_path)),
        e_extension(extension),
        s_contents(std::move(contents)) {
}

/**
 * The two supported file types are dictated here.
 * All other file types are rejected.
 */
std::string_view toString (const Extension type) {
	switch (type) {
	case ClothObject:
		return "co";
	case Library:
		return "clib";
	default:
		return "UNKNOWN";
	}
}

std::string SourceFile::sourceName (const std::filesystem::path &path) {
	if (const std::string s_name = pathUtf8(path.stem()); !s_name.empty() || s_name != SEPERATOR) {
		return s_name;
	}
	throw std::invalid_argument("Invalid source file name '" + pathUtf8(path) + "'");
}

std::string lowercaseExtension (const std::filesystem::path &path) {
	std::string s_suffix = pathUtf8(path.extension());
	for (char &c : s_suffix) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	return s_suffix;
}

Extension SourceFile::sourceExtension (const std::filesystem::path &path) {
	if (const std::string s_suffix = lowercaseExtension(path); !s_suffix.empty()) {
		const std::string_view s_spelling = std::string_view{ s_suffix }.substr(1);
		for (const auto extension : { ClothObject, Library }) {
			if (s_spelling == toString(extension)) {
				return extension;
			}
		}
	}

	throw std::invalid_argument("Unsupported extension for source file '" + pathUtf8(path) + "' (expected .co or .clib)");
}

std::string SourceFile::readSource (const std::filesystem::path &path) {
	if (!std::filesystem::is_regular_file(path)) {
		throw std::runtime_error("Could not open source file '" + pathUtf8(path) + "': expected a regular file");
	}

	std::ifstream ifs_input(path, std::ios::binary);
	if (!ifs_input) {
		throw std::runtime_error("Could not open source file '" + pathUtf8(path) + "'");
	}

	std::string s_contents;
	char        c_buffer[4096];
	while (ifs_input.read(c_buffer, sizeof(c_buffer))) {
		s_contents.append(c_buffer, sizeof(c_buffer));
	}
	if (ifs_input.bad() || !ifs_input.eof()) {
		throw std::runtime_error("Could not read source file '" + pathUtf8(path) + "'");
	}
	s_contents.append(c_buffer, static_cast<std::size_t>(ifs_input.gcount()));

	if (const std::size_t st_valid = utf8ValidPrefixLength(s_contents); st_valid != s_contents.size()) {
		throw std::runtime_error("Invalid UTF-8 in source file '" + pathUtf8(path) + "' at byte " + std::to_string(st_valid + 1));
	}
	return s_contents;
}

}
