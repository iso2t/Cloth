// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cloth {

#define SEPERATOR "."

enum Extension { ClothObject, Library };

std::string_view toString (Extension type);
std::string      pathUtf8 (const std::filesystem::path &path);
std::string      lowercaseExtension (const std::filesystem::path &path);

class SourceFile {
    public:
	explicit SourceFile (std::filesystem::path path);
	SourceFile (std::filesystem::path path, std::string contents, Extension extension = ClothObject);

	[[nodiscard]] std::string getName () const {
		return s_name;
	}

	[[nodiscard]] const std::filesystem::path &getPath () const noexcept {
		return p_path;
	}

	[[nodiscard]] const std::string &getContents () const & noexcept {
		return s_contents;
	}

	[[nodiscard]] std::string getContents () const && {
		return s_contents;
	}

	[[nodiscard]] Extension getExtension () const {
		return e_extension;
	}

    private:
	std::filesystem::path p_path;
	std::string           s_name;
	Extension             e_extension;
	std::string           s_contents;

	static std::string sourceName (const std::filesystem::path &path);
	static Extension   sourceExtension (const std::filesystem::path &path);
	static std::string readSource (const std::filesystem::path &path);
};

}
