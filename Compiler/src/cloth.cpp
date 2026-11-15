// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <file/source_file.hpp>
#include <lexer/Lexer.hpp>
#include <util/pstream.hpp>
#include <system/compiler_options.hpp>
#include <errors/cloth_errors.hpp>

#include <exception>
#include <iostream>

int main(int argc, char *argv[]) try {
	const cloth::sys::CompilerOptions options{ argc, argv };

	if (!options.source_path)
		return FAILED;
	auto lexer = cloth::Lexer{ cloth::SourceFile{ *options.source_path } };
	if (options.get_lexer_flag() == 1) {
		lexer.scanTokens(); // TODO: discards result, not capturing the vector.
		return LEXED_ONLY;
	}

	if (options.get_lexer_flag() == 2) {
		cloth::printLexer(lexer, std::cout);
		return PRINT_LEXED_ONLY;
	}

	return SUCCESS;
} catch (const std::exception &error) {
	std::cerr << error.what() << '\n';
	return FAILED;
}
