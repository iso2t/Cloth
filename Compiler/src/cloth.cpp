// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>

#include <exception>
#include <iostream>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <cde/cloth_diagnostic.hpp>
#include <cde/diagnostic_reporter.hpp>
#include <errors/cloth_errors.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>
#include <system/compiler_options.hpp>
#include <util/pstream.hpp>

#ifdef _WIN32
int wmain (const int argc, wchar_t *wideArgv[]) try {
	// The Windows narrow entry point loses characters outside the active code page.
	std::vector<std::string>  v_utf8;
	std::vector<const char *> v_arguments;
	v_utf8.reserve(argc);
	v_arguments.reserve(argc);
	for (int i_index = 0; i_index < argc; ++i_index) {
		v_arguments.push_back(v_utf8.emplace_back(cloth::pathUtf8(wideArgv[i_index])).c_str());
	}
	const char *const *argv = v_arguments.data();
#else
int main (const int argc, char *argv[]) try {
#endif
	const cloth::sys::CompilerOptions co_options{ argc, argv };
	if (co_options.is_empty()) {
		std::fprintf(stderr, "incorrect usage: cloth <args>");
	}

	if (!co_options.p_sourcePath) return FAILED;

	auto l_lexer = cloth::Lexer{ cloth::SourceFile{ *co_options.p_sourcePath } };
	if (co_options.get_lexer_flag() == 1) {
		std::ignore = l_lexer.scanTokens(); // discards result, not capturing the vector.
		return LEXED_ONLY;
	}

	if (co_options.get_lexer_flag() == 2) {
		cloth::printLexer(l_lexer, std::cout);
		return PRINT_LEXED_ONLY;
	}

	cloth::Parser      p_parser{ l_lexer };
	cloth::SymbolTable st_symbols{ p_parser.parse() };
	std::ignore = backend::BackendObject{ cloth::CheckedProgram{ std::move(st_symbols), co_options.targetLayout, co_options.standardLibraryRoot,
		                                                     co_options.kind } };

	return SUCCESS;
} catch (const cde::ClothDiagnostic &error) {
	cde::DiagnosticReporter{ std::cerr }.report(error);
	return FAILED;
} catch (const std::exception &error) {
	std::cerr << error.what() << '\n';
	return FAILED;
}
