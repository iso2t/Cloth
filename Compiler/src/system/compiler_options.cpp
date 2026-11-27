// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/Flags.rst

#include <backend/backend.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <ostream>
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

#ifdef _WIN32
#include <errhandlingapi.h>
#include <libloaderapi.h>
#include <minwindef.h>
#include <system_error>
#elif defined(__APPLE__)
#include <cstdint>

#include <mach-o/dyld.h>
#endif

namespace cloth::sys {
namespace {
std::filesystem::path compilerExecutable () {
#ifdef _WIN32
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Could not locate the Cloth compiler");
		if (length < path.size()) {
			path.resize(length);
			return std::filesystem::canonical(path);
		}
		path.resize(path.size() * 2);
	}
#elif defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> path(size);
	if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("Could not locate the Cloth compiler");
	return std::filesystem::canonical(path.data());
#else
	return std::filesystem::canonical("/proc/self/exe");
#endif
}

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
				throw cde::ClothDiagnostic(cde::DiagnosticCode::MissingOptionValue, bi_flag->s_flag + " requires " + rr_value->s_description);
			}
			rr_value->f_func(argv[++index]);
		} else {
			std::get<std::function<void()>>(bi_flag->v_action)();
		}
	}
	if (action == DriverAction::Help || action == DriverAction::Version) return;
	if (action == DriverAction::PrintTarget && (p_sourcePath || !s_emitFlag.empty() || i_lexer))
		throw std::invalid_argument("--print-target takes no source or output");
	if (modules.recordExternInterfaces && (!p_sourcePath || i_lexer))
		throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidCompilationRequest, "--record-extern-interfaces requires a checked --src PATH");
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
	if (p_sourcePath && !i_lexer && standardLibraryRoot.empty()) standardLibraryRoot = compilerExecutable().parent_path() / "lib/cloth/stdlib";
}

void CompilerOptions::addFlag (const CompilerFlag &flag) {
	v_compilerFlags.push_back(flag);
}

void CompilerOptions::printHelp (std::ostream &output) const {
	output << "Usage\n"
	          "  cloth [options] --src PATH\n"
	          "  cloth --print-target [--target-bits 32|64]\n"
	          "  cloth --help\n"
	          "  cloth --version\n\n"
	          "Options\n";
	for (const CompilerFlag &flag : v_compilerFlags) {
		std::string usage = flag.s_flag;
		if (const auto *value = std::get_if<CompilerFlag::Value>(&flag.v_action)) usage += std::format(" <{}>", value->s_description);
		output << std::format("  {:<32}  {}\n", usage, flag.s_help);
	}
}

// TODO: Allow comma separated lists of source files to use all at the same time? ie "path1","path2"
// TODO: "path1","path" "path3" fails as path3 is seen as another arg.
void CompilerOptions::buildDefaults () {
	addFlag({ .s_flag = "--help", .s_help = "Show this help.", .v_action = [this] { action = DriverAction::Help; } });
	addFlag({ .s_flag = "--version", .s_help = "Print the compiler version.", .v_action = [this] { action = DriverAction::Version; } });
	addFlag({ .s_flag = "--print-target", .s_help = "Print the target triple.", .v_action = [this] { action = DriverAction::PrintTarget; } });
	addFlag({ .s_flag = "--record-extern-interfaces", .s_help = "Record the native interfaces of external libraries.", .v_action = [this] {
		         modules.recordExternInterfaces = true;
	         } });
	addFlag({ .s_flag   = "--module-name",
	          .s_help   = "Name the library being compiled.",
	          .v_action = CompilerFlag::Value{ .s_description = "a library name",
	                                           .f_func        = [this] (std::string_view name) { modules.name = moduleName(name); } } });
	addFlag({ .s_flag   = "--extern",
	          .s_help   = "Bind a separately compiled library to its source.",
	          .v_action = CompilerFlag::Value{
	                  .s_description = "NAME=SOURCE", .f_func = [this] (std::string_view binding) {
		                  const auto st_equal = binding.find('=');
		                  if (st_equal == binding.npos || st_equal + 1 == binding.size()) throw std::invalid_argument("--extern requires NAME=SOURCE");
		                  const auto path = binding.substr(st_equal + 1);
		                  if (!modules.imports.emplace(moduleName(binding.substr(0, st_equal)), std::u8string(path.begin(), path.end())).second)
			                  throw std::invalid_argument("Duplicate --extern module name");
	                  } } });
	addFlag({ .s_flag   = "--kind",
	          .s_help   = "Select the program kind (library by default).",
	          .v_action = CompilerFlag::Value{ .s_description = "library or executable", .f_func = [this] (const std::string_view kindName) {
		                                          if (kindName == "library")
			                                          kind = ProgramKind::Library;
		                                          else if (kindName == "executable")
			                                          kind = ProgramKind::Executable;
		                                          else
			                                          throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidOptionValue,
			                                                                     "--kind requires library or executable");
	                                          } } });
	addFlag({ .s_flag   = "--target-bits",
	          .s_help   = "Set the target pointer width.",
	          .v_action = CompilerFlag::Value{ .s_description = "32 or 64", .f_func = [this] (const std::string_view bits) {
		                                          if (bits == "32")
			                                          targetLayout.sizeBits = 32;
		                                          else if (bits == "64")
			                                          targetLayout.sizeBits = 64;
		                                          else
			                                          throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidOptionValue,
			                                                                     "--target-bits requires 32 or 64");
	                                          } } });
	addFlag({ .s_flag   = "--stdlib",
	          .s_help   = "Set the standard library source directory.",
	          .v_action = CompilerFlag::Value{ .s_description = "a directory", .f_func = [this] (const std::string_view path) {
		                                          standardLibraryRoot = std::u8string(path.begin(), path.end());
	                                          } } });
	addFlag({ .s_flag = "-l", .s_help = "Scan tokens and exit silently.", .v_action = [this] { i_lexer = 1; } });
	addFlag({ .s_flag = "-L", .s_help = "Print the token table and exit.", .v_action = [this] { i_lexer = 2; } });
	addFlag({ .s_flag   = "--src",
	          .s_help   = "Select the entry source file.",
	          .v_action = CompilerFlag::Value{ .s_description = "a file path", .f_func = [this] (const std::string_view path) {
		                                          p_sourcePath.emplace(std::u8string(path.begin(), path.end()));
	                                          } } });
	addFlag({ .s_flag   = "--emit",
	          .s_help   = "Choose the native output format.",
	          .v_action = CompilerFlag::Value{ .s_description = "object or assembly", .f_func = [this] (const std::string_view name) {
		                                          if (name == "object")
			                                          emitKind = backend::EmitKind::Object;
		                                          else if (name == "assembly")
			                                          emitKind = backend::EmitKind::Assembly;
		                                          else
			                                          throw cde::ClothDiagnostic(cde::DiagnosticCode::InvalidOptionValue,
			                                                                     "--emit requires object or assembly");
		                                          s_emitFlag = "--emit";
	                                          } } });
	for (const char *flag : { "-o", "--output", "--emit-object" }) {
		addFlag({ .s_flag   = flag,
		          .s_help   = "Set the output file path.",
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
