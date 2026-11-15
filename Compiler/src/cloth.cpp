#include <file/source_file.hpp>
#include <lexer/Lexer.hpp>
#include <util/pstream.hpp>
#include <system/compiler_options.hpp>
#include <errors/cloth_errors.hpp>

#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
	cloth::sys::CompilerOptions options{ argc, argv };
	std::string textSource = R"(
        using lexer.Lexer: *
        using parser.Ast: SourceUnit
        using parser.Parser: Parser
        using source.SourceFile: SourceFile
        using token.TokenList: TokenList
        using cloth.io.fs.File: File, FileError
        using cloth.io.path.Path: pathOf
        using cloth.text.Utf8: decodeUtf8

        fn<isize> parseSource(Lexer lexer) {
            def<TokenList?> tokens := lexer.scanTokens()
            if (tokens == null) {
                if (lexer.failure != null) println(lexer.failure)
                return 1
            }

            def parser := Parser::new(tokens)
            def<SourceUnit?> unit := parser.parse()
            def<isize> status = 0
            if (unit == null) {
                if (parser.failure != null) println(parser.failure)
                status = 1
            } else {
                println(unit)
                unit::delete
            }
            parser::delete
            tokens::delete
            return status
        }

    )";

	cloth::SourceFile co("Main", textSource, cloth::Extension::CLOTH_OBJECT);
	cloth::Lexer lexer(co);
	if (options.LEXER == 1) {
		lexer.scanTokens(); // TODO: discards result, not capturing the vector.
		return LEXED_ONLY;
	}

	if (options.LEXER == 2) {
		cloth::printLexer(lexer, std::cout);
		return PRINT_LEXED_ONLY;
	}

	return SUCCESS;
}
