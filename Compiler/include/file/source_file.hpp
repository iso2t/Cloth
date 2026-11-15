// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cloth {

#define SEPERATOR "."

enum Extension { CLOTH_OBJECT, LIBRARY };

std::string_view to_string(Extension type);

class SourceFile {
    public:
	explicit SourceFile(const std::filesystem::path &path);

	~SourceFile() = default;

	std::string get_name() const {
		return s_name;
	}

	std::string get_contents() const {
		return s_contents;
	}

	Extension get_extension() const {
		return e_extension;
	}

    private:
	const std::string s_name;
	const Extension e_extension;
	const std::string s_contents;

	std::string source_name(const std::filesystem::path &path);
	Extension source_extension(const std::filesystem::path &path);
	std::string read_source(const std::filesystem::path &path);
};

}
