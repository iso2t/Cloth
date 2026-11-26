// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/Flags.rst

#include <backend/backend.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <diagnostics.hpp>
#include <lexer/lexer.hpp>
#include <semantic/program_kind.hpp>
#include <system/compiler_options.hpp>
#include <token/token.hpp>

namespace cloth::sys {
namespace {
std::string moduleName (std::string_view name) {
	const std::vector<Token> v_tokens = Lexer(std::string(name)).scanTokens();
	if (v_tokens.size() != 2 || v_tokens.front().getType() != TokenType::Identifier || v_tokens.front().getLexeme() != name)
		throw std::invalid_argument("A library module name must be a Cloth identifier");
	return v_tokens.front().getNameKey();
}
}

CompilerOptions::CompilerOptions (const int argc, const char *const argv[]) : args(argc) {
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
	if (printTarget && (p_sourcePath || !s_emitFlag.empty() || i_lexer)) throw std::invalid_argument("--print-target takes no source or output");
	if (!modules.name.empty() && (kind != ProgramKind::Library || modules.imports.contains(modules.name)))
		throw std::invalid_argument("--module-name requires a library name distinct from its dependencies");
	if (!s_emitFlag.empty()) {
		if (kind == ProgramKind::Library && modules.name.empty())
			throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest,
			                           std::format("{} requires --module-name for library output", s_emitFlag));
		if (i_lexer != 0)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest,
			                           std::format("{} is incompatible with lexer-only mode (-l or -L)", s_emitFlag));
		if (!p_sourcePath)
			throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest, std::format("{} requires --src PATH", s_emitFlag));
		if (!p_outputPath) p_outputPath.emplace(*p_sourcePath).replace_extension(emitKind == backend::EmitKind::Assembly ? ".s" : ".o");
	}
}

void CompilerOptions::addFlag (const CompilerFlag &flag) {
	v_compilerFlags.push_back(flag);
}

// TODO: Allow comma separated lists of source files to use all at the same time? ie "path1","path2"
// TODO: "path1","path" "path3" fails as path3 is seen as another arg.
void CompilerOptions::buildDefaults () {
	addFlag({ .s_flag = "--print-target", .v_action = [this] { printTarget = true; } });
	addFlag({ .s_flag = "--module-name", .v_action = CompilerFlag::Value{ .s_description = "a library name", .f_func = [this] (std::string_view name) {
		                                                                     modules.name = moduleName(name);
	                                                                     } } });
	addFlag({ .s_flag   = "--extern",
	          .v_action = CompilerFlag::Value{
	                  .s_description = "NAME=SOURCE", .f_func = [this] (std::string_view binding) {
		                  const auto st_equal = binding.find('=');
		                  if (st_equal == binding.npos || st_equal + 1 == binding.size()) throw std::invalid_argument("--extern requires NAME=SOURCE");
		                  const auto path = binding.substr(st_equal + 1);
		                  if (!modules.imports.emplace(moduleName(binding.substr(0, st_equal)), std::u8string(path.begin(), path.end())).second)
			                  throw std::invalid_argument("Duplicate --extern module name");
	                  } } });
	addFlag({ .s_flag   = "--kind",
	          .v_action = CompilerFlag::Value{ .s_description = "library or executable", .f_func = [this] (const std::string_view kindName) {
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
	addFlag({ .s_flag = "-l", .v_action = [this] { i_lexer = 1; } });
	addFlag({ .s_flag = "-L", .v_action = [this] { i_lexer = 2; } });
	addFlag({ .s_flag = "--src", .v_action = CompilerFlag::Value{ .s_description = "a file path", .f_func = [this] (const std::string_view path) {
		                                                             p_sourcePath.emplace(std::u8string(path.begin(), path.end()));
	                                                             } } });
	addFlag({ .s_flag = "--emit", .v_action = CompilerFlag::Value{ .s_description = "object or assembly", .f_func = [this] (const std::string_view name) {
		                                                              if (name == "object")
			                                                              emitKind = backend::EmitKind::Object;
		                                                              else if (name == "assembly")
			                                                              emitKind = backend::EmitKind::Assembly;
		                                                              else
			                                                              throw std::invalid_argument("--emit requires object or assembly");
		                                                              s_emitFlag = "--emit";
	                                                              } } });
	for (const char *flag : { "-o", "--output", "--emit-object" }) {
		addFlag({ .s_flag   = flag,
		          .v_action = CompilerFlag::Value{ .s_description = "a file path", .f_func = [this, flag] (const std::string_view path) {
			                                          p_outputPath.emplace(std::u8string(path.begin(), path.end()));
			                                          if (s_emitFlag.empty()) s_emitFlag = flag;
		                                          } } });
	}
}

bool CompilerOptions::isEmpty () const {
	return args < 2;
}

}
