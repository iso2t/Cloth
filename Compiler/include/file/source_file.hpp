#pragma once

#include <string>
#include <string_view>

namespace cloth {

#define SEPERATOR "."

enum Extension { CLOTH_OBJECT, LIBRARY };

std::string_view to_string(Extension type);

class SourceFile {
    public:
	SourceFile(const std::string &name, const std::string &contents, Extension extension)
		: s_name(name)
		, s_contents(contents)
		, e_extension(extension) {
	}

	~SourceFile() = default;

	std::string name() const {
		return s_name;
	}

	std::string contents() const {
		return s_contents;
	}

	Extension extension() const {
		return e_extension;
	}

    private:
	const std::string s_name;
	const std::string s_contents;
	const Extension e_extension;
};

}
