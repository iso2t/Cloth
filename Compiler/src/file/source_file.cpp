// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <file/source_file.hpp>
#include <util/utf8.hpp>

#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <stdexcept>

namespace cloth {

SourceFile::SourceFile(const std::filesystem::path &path)
	: s_name(source_name(path))
	, e_extension(source_extension(path))
	, s_contents(read_source(path)) {
}

/**
 * The two supported file types are dictated here.
 * All other file types are rejected.
 */
std::string_view to_string(Extension type) {
	switch (type) {
	case CLOTH_OBJECT:
		return "co";
	case LIBRARY:
		return "clib";
	default:
		return "UNKNOWN";
	}
}

std::string SourceFile::source_name(const std::filesystem::path &path) {
	const auto name = path.stem().string();
	if (name.empty() || name == SEPERATOR) {
		throw std::invalid_argument("Invalid source file name '" + path.string() + "'");
	}
	return name;
}

Extension SourceFile::source_extension(const std::filesystem::path &path) {
	auto suffix = path.extension().string();
	if (!suffix.empty()) {
		for (char &c : suffix) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}

		const auto spelling = std::string_view{ suffix }.substr(1);
		for (const auto extension : { CLOTH_OBJECT, LIBRARY }) {
			if (spelling == to_string(extension)) {
				return extension;
			}
		}
	}

	throw std::invalid_argument("Unsupported extension for source file '" + path.string() + "' (expected .co or .clib)");
}

std::string SourceFile::read_source(const std::filesystem::path &path) {
	if (!std::filesystem::is_regular_file(path)) {
		throw std::runtime_error("Could not open source file '" + path.string() + "': expected a regular file");
	}

	std::ifstream input(path, std::ios::binary);
	if (!input) {
		throw std::runtime_error("Could not open source file '" + path.string() + "'");
	}

	std::string contents;
	char buffer[4096];
	while (input.read(buffer, sizeof(buffer))) {
		contents.append(buffer, sizeof(buffer));
	}
	if (input.bad() || !input.eof()) {
		throw std::runtime_error("Could not read source file '" + path.string() + "'");
	}
	contents.append(buffer, static_cast<std::size_t>(input.gcount()));

	for (auto bytes = std::string_view{ contents }; !bytes.empty();) {
		const auto length = utf8_scalar_length(bytes);
		if (length == 0) {
			const auto offset = contents.size() - bytes.size() + 1;
			throw std::runtime_error("Invalid UTF-8 in source file '" + path.string() + "' at byte " + std::to_string(offset));
		}
		bytes.remove_prefix(length);
	}
	return contents;
}

}
