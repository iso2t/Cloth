// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <exception>
#include <iostream>
#include <tuple>

#include <cloth_diagnostic.hpp>
#include <diagnostic_reporter.hpp>
#include <errors/cloth_errors.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <system/compiler_options.hpp>
#include <util/pstream.hpp>

int main (int argc, char *argv[]) try {
	const cloth::sys::CompilerOptions co_options{ argc, argv };

	if (!co_options.p_sourcePath) return FAILED;

	cloth::Lexer l_lexer = cloth::Lexer{ cloth::SourceFile{ *co_options.p_sourcePath } };
	if (co_options.get_lexer_flag() == 1) {
		std::ignore = l_lexer.scanTokens(); // discards result, not capturing the vector.
		return LEXED_ONLY;
	}

	if (co_options.get_lexer_flag() == 2) {
		cloth::printLexer(l_lexer, std::cout);
		return PRINT_LEXED_ONLY;
	}

	cloth::Parser p_parser = cloth::Parser{ l_lexer };
	p_parser.parse();

	std::cout << "Success" << std::endl;

	return SUCCESS;
} catch (const cde::ClothDiagnostic &error) {
	cde::DiagnosticReporter{ std::cerr }.report(error);
	return FAILED;
} catch (const std::exception &error) {
	std::cerr << error.what() << '\n';
	return FAILED;
}
