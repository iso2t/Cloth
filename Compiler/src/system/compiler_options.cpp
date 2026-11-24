// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/Flags.rst

#include <algorithm>
#include <format>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <variant>

#include <diagnostics.hpp>
#include <semantic/program_kind.hpp>
#include <system/compiler_options.hpp>

namespace cloth::sys {

CompilerOptions::CompilerOptions (const int argc, const char *const argv[]) : args(argc), argv(argv) {
	buildDefaults();

	for (int index = 1; index < argc; ++index) {
		const std::string_view s_argument = argv[index];
		const auto             bi_flag    = std::ranges::find(v_compilerFlags, s_argument, &CompilerFlag::s_flag);
		if (bi_flag == v_compilerFlags.end()) {
			if (s_argument.starts_with('-')) {
				throw cde::ClothDiagnostic(cde::DiagnosticCode::UnknownOption, std::format("Unknown option '{}'", s_argument));
			}
			throw cde::ClothDiagnostic(cde::DiagnosticCode::TooManyArguments, std::format("Unexpected positional argument '{}'", s_argument));
		}

		if (const auto *rr_value = std::get_if<CompilerFlag::Value>(&bi_flag->v_action)) {
			if (index + 1 == argc || argv[index + 1][0] == '\0') {
				throw std::invalid_argument(bi_flag->s_flag + " requires " + rr_value->s_description);
			}
			rr_value->f_func(argv[++index]);
		} else {
			std::get<std::function<void()>>(bi_flag->v_action)();
		}
	}
	if (p_objectPath) {
		if (kind != ProgramKind::Executable)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest, "--emit-object requires --kind executable");
		if (i_lexer != 0)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest,
			                           "--emit-object is incompatible with lexer-only mode (-l or -L)");
		if (!p_sourcePath) throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest, "--emit-object requires --src PATH");
	}
}

void CompilerOptions::addFlag (const CompilerFlag &flag) {
	v_compilerFlags.push_back(flag);
}

// TODO: Allow comma separated lists of source files to use all at the same time? ie "path1","path2"
// TODO: "path1","path" "path3" fails as path3 is seen as another arg.
void CompilerOptions::buildDefaults () {
	addFlag({ .s_flag = "--kind", .v_action = CompilerFlag::Value{ .s_description = "library or executable", .f_func = [this] (std::string_view kindName) {
		                                                              if (kindName == "library")
			                                                              kind = ProgramKind::Library;
		                                                              else if (kindName == "executable")
			                                                              kind = ProgramKind::Executable;
		                                                              else
			                                                              throw std::invalid_argument("--kind requires library or executable");
	                                                              } } });
	addFlag({ .s_flag = "--target-bits", .v_action = CompilerFlag::Value{ .s_description = "32 or 64", .f_func = [this] (const std::string_view bits) {
		                                                                     if (bits == "32")
			                                                                     targetLayout.sizeBits = 32;
		                                                                     else if (bits == "64")
			                                                                     targetLayout.sizeBits = 64;
		                                                                     else
			                                                                     throw std::invalid_argument("--target-bits requires 32 or 64");
	                                                                     } } });
	addFlag({ .s_flag = "--stdlib", .v_action = CompilerFlag::Value{ .s_description = "a directory", .f_func = [this] (const std::string_view path) {
		                                                                standardLibraryRoot = std::u8string(path.begin(), path.end());
	                                                                } } });
	addFlag({ .s_flag = "-l", .v_action = [this] () { i_lexer = 1; } });
	addFlag({ .s_flag = "-L", .v_action = [this] () { i_lexer = 2; } });
	addFlag({ .s_flag = "--src", .v_action = CompilerFlag::Value{ .s_description = "a file path", .f_func = [this] (std::string_view path) {
		                                                             p_sourcePath.emplace(std::u8string(path.begin(), path.end()));
	                                                             } } });
	addFlag({ .s_flag = "--emit-object", .v_action = CompilerFlag::Value{ .s_description = "a file path", .f_func = [this] (std::string_view path) {
		                                                                     p_objectPath.emplace(std::u8string(path.begin(), path.end()));
	                                                                     } } });
}

bool CompilerOptions::isEmpty () const {
	return args < 2;
}

}
