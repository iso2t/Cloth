// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>

#include <cde/cloth_diagnostic.hpp>
#include <cde/diagnostic_code.hpp>
#include <system/compiler_options.hpp>

namespace cloth::sys {

CompilerOptions::CompilerOptions (const int argc, const char *const argv[]) {
	this->args = argc;
	this->argv = argv;
	build_defaults();

	for (int index = 1; index < argc; ++index) {
		const std::string_view s_argument = argv[index];
		const auto             bi_flag    = std::ranges::find(v_compilerFlags, s_argument, &CompilerFlag::s_flag);
		if (bi_flag == v_compilerFlags.end()) {
			if (s_argument.starts_with('-')) {
				throw cde::ClothDiagnostic(cde::DiagnosticCode::UNKNOWN_OPTION, std::format("Unknown option '{}'", s_argument));
			}
			throw cde::ClothDiagnostic(cde::DiagnosticCode::TOO_MANY_ARGUMENTS, std::format("Unexpected positional argument '{}'", s_argument));
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
			throw cde::ClothDiagnostic(cde::DiagnosticCode::INVALID_COMPILATION_REQUEST, "--emit-object requires --kind executable");
		if (i_lexer != 0)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::INVALID_COMPILATION_REQUEST,
			                           "--emit-object is incompatible with lexer-only mode (-l or -L)");
		if (!p_sourcePath) throw cde::ClothDiagnostic(cde::DiagnosticCode::INVALID_COMPILATION_REQUEST, "--emit-object requires --src PATH");
	}
}

void CompilerOptions::add_flag (const CompilerFlag &flag) {
	v_compilerFlags.push_back(flag);
}

// TODO: Allow comma separated lists of source files to use all at the same time? ie "path1","path2"
// TODO: "path1","path" "path3" fails as path3 is seen as another arg.
void CompilerOptions::build_defaults () {
	add_flag({ "--kind", CompilerFlag::Value{ "library or executable", [this] (std::string_view kindName) {
		                                         if (kindName == "library")
			                                         kind = ProgramKind::Library;
		                                         else if (kindName == "executable")
			                                         kind = ProgramKind::Executable;
		                                         else
			                                         throw std::invalid_argument("--kind requires library or executable");
	                                         } } });
	add_flag({ "--target-bits", CompilerFlag::Value{ "32 or 64", [this] (const std::string_view bits) {
		                                                if (bits == "32")
			                                                targetLayout.sizeBits = 32;
		                                                else if (bits == "64")
			                                                targetLayout.sizeBits = 64;
		                                                else
			                                                throw std::invalid_argument("--target-bits requires 32 or 64");
	                                                } } });
	add_flag({ "--stdlib", CompilerFlag::Value{ .s_description = "a directory", .f_func = [this] (const std::string_view path) {
		                                           standardLibraryRoot = std::u8string(path.begin(), path.end());
	                                           } } });
	add_flag({ .s_flag = "-l", .v_action = [this] () { i_lexer = 1; } });
	add_flag({ .s_flag = "-L", .v_action = [this] () { i_lexer = 2; } });
	add_flag({ .s_flag = "--src", .v_action = CompilerFlag::Value{ .s_description = "a file path", .f_func = [this] (std::string_view path) {
		                                                              p_sourcePath.emplace(std::u8string(path.begin(), path.end()));
	                                                              } } });
	add_flag({ "--emit-object",
	           CompilerFlag::Value{ "a file path", [this] (std::string_view path) { p_objectPath.emplace(std::u8string(path.begin(), path.end())); } } });
}

bool CompilerOptions::is_empty () const {
	return args < 2;
}

}
