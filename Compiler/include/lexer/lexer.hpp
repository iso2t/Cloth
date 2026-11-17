// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <file/source_file.hpp>
#include <token/token.hpp>

namespace cloth {
struct Position {
	size_t line;
	size_t column;
};

class Lexer {
    public:
	Lexer (std::string src) : Lexer(SourceFile{ "<input>", std::move(src) }) {
	}

	Lexer (SourceFile source) : p_source(std::make_shared<const SourceFile>(std::move(source))) {
	}

	Lexer (const char *src) : Lexer(std::string(src)) {
	}

	Lexer (const char *src, size_t length) : Lexer(std::string(src, length)) {
	}

	Lexer (const SourceFile &source, size_t offset) : Lexer(SourceFile{ source.get_path(), source.get_contents().substr(offset), source.get_extension() }) {
	}

	~Lexer () = default;

	std::vector<Token> scanTokens ();
	void               scan ();

	std::vector<Token> get_tokens () const {
		return v_tokens;
	}

    private:
	std::shared_ptr<const SourceFile> p_source;
	std::vector<Token>                v_tokens;
	int                               i_start   = 0;
	int                               i_current = 0;

	Position p_start{ 1, 1 };
	Position p_current{ 1, 1 };

	const std::string &source_text () const noexcept {
		return p_source->get_contents();
	}

	char            advance ();
	char            peek () const;
	bool            isAtEnd () const;
	void            addToken (TokenType type, TokenSpan span, const std::string &lexeme);
	TokenSpan       span ();
	TokenSpan       cursorSpan ();
	cloth::Operator matchOperator ();
	void            scanToken ();
	bool            skipComments ();
	void            number ();
	void            identifier ();
	void            interpolatedString ();
	void            rawString ();
	void            advanceStringScalar ();
	void            string ();
	void            character ();
};
}
