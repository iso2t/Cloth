// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <semantic/program_kind.hpp>
#include <semantic/target_layout.hpp>

namespace cloth::sys {

struct CompilerFlag {
	struct Value {
		std::string                           s_description;
		std::function<void(std::string_view)> f_func;
	};

	std::string                                s_flag;
	std::variant<std::function<void()>, Value> v_action;
};

class CompilerOptions {
    public:
	CompilerOptions (int argc, const char *const argv[]);

	~CompilerOptions () = default;

	[[nodiscard]] std::vector<CompilerFlag> getFlags () const {
		return v_compilerFlags;
	}

	void addFlag (const CompilerFlag &flag);
	void buildDefaults ();

	std::optional<std::filesystem::path> p_sourcePath;
	std::optional<std::filesystem::path> p_objectPath;
	TargetLayout                         targetLayout;
	std::filesystem::path                standardLibraryRoot;
	ProgramKind                          kind = ProgramKind::Library;

	[[nodiscard]] int getLexerFlag () const {
		return i_lexer;
	}

	[[nodiscard]] bool isEmpty () const;

    private:
	std::vector<CompilerFlag> v_compilerFlags;
	int                       i_lexer = 0;
	int                       args{};
	const char *const        *argv{};
};

}
