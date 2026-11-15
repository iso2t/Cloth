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
		std::string description;
		std::function<void(std::string_view)> func;
	};

	std::string flag;
	std::variant<std::function<void()>, Value> action;
};

class CompilerOptions {
    public:
	CompilerOptions(int argc, const char *const argv[]);

	~CompilerOptions() = default;

	std::vector<CompilerFlag> get_flags() const {
		return compiler_flags;
	}

	void add_flag(const CompilerFlag &flag);
	void build_defaults();

	std::optional<std::filesystem::path> source_path;

	int get_lexer_flag() const {
		return LEXER;
	}

    private:
	std::vector<CompilerFlag> compiler_flags;
	int LEXER = 0;
};

}
