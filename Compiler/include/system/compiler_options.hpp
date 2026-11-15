// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cloth::sys {

struct CompilerFlag {
	struct Value {
		std::string s_description;
		std::function<void(std::string_view)> f_func;
	};

	std::string s_flag;
	std::variant<std::function<void()>, Value> v_action;
};

class CompilerOptions {
    public:
	CompilerOptions(int argc, const char *const argv[]);

	~CompilerOptions() = default;

	std::vector<CompilerFlag> get_flags() const {
		return v_compilerFlags;
	}

	void add_flag(const CompilerFlag &flag);
	void build_defaults();

	std::optional<std::filesystem::path> p_sourcePath;

	int get_lexer_flag() const {
		return i_lexer;
	}

    private:
	std::vector<CompilerFlag> v_compilerFlags;
	int i_lexer = 0;
};

}
