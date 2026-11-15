// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <file/source_file.hpp>
#include <lexer/Lexer.hpp>
#include <util/pstream.hpp>
#include <system/compiler_options.hpp>
#include <errors/cloth_errors.hpp>

#include <exception>
#include <iostream>

int main(int argc, char *argv[]) try {
	const cloth::sys::CompilerOptions co_options{ argc, argv };

	if (!co_options.p_sourcePath)
		return FAILED;

	cloth::Lexer l_lexer = cloth::Lexer{ cloth::SourceFile{ *co_options.p_sourcePath } };
	if (co_options.get_lexer_flag() == 1) {
		l_lexer.scanTokens(); // TODO: discards result, not capturing the vector.
		return LEXED_ONLY;
	}

	if (co_options.get_lexer_flag() == 2) {
		cloth::printLexer(l_lexer, std::cout);
		return PRINT_LEXED_ONLY;
	}

	return SUCCESS;
} catch (const std::exception &error) {
	std::cerr << error.what() << '\n';
	return FAILED;
}
