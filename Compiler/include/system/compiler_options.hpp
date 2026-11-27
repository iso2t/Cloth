// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <backend/backend.hpp>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <semantic/native_module.hpp>
#include <semantic/program_kind.hpp>
#include <semantic/target_layout.hpp>

namespace cloth::sys {

enum class DriverAction { Compile, Help, Version, PrintTarget };

struct CompilerFlag {
	struct Value {
		std::string                           s_description;
		std::function<void(std::string_view)> f_func;
	};

	std::string                                s_flag;
	std::string_view                           s_help;
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
	void printHelp (std::ostream &output) const;

	std::optional<std::filesystem::path> p_sourcePath;
	std::optional<std::filesystem::path> p_outputPath;
	backend::EmitKind                    emitKind = backend::EmitKind::Object;
	TargetLayout                         targetLayout;
	std::filesystem::path                standardLibraryRoot;
	ProgramKind                          kind = ProgramKind::Library;
	NativeModules                        modules;
	DriverAction                         action = DriverAction::Compile;

	[[nodiscard]] int getLexerFlag () const {
		return i_lexer;
	}

	[[nodiscard]] bool isEmpty () const;

    private:
	std::vector<CompilerFlag> v_compilerFlags;
	std::string_view          s_emitFlag;
	int                       i_lexer = 0;
	int                       args{};
};

}
