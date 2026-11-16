// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

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
	Lexer (const std::string &src) : s_source(src) {
	}

	Lexer (const SourceFile &co) : s_source(co.get_contents()) {
	}

	Lexer (const char *src) : s_source(src) {
	}

	Lexer (const char *src, size_t length) :
	        s_source(std::string(src, length)) {
	}

	Lexer (const SourceFile &co, size_t length) :
	        s_source(std::string(co.get_contents(), length)) {
	}

	~Lexer () = default;

	std::vector<Token> scanTokens ();
	void               scan ();

	std::vector<Token> get_tokens () const {
		return v_tokens;
	}

    private:
	std::string        s_source;
	std::vector<Token> v_tokens;
	int                i_start   = 0;
	int                i_current = 0;

	Position p_start{ 1, 1 };
	Position p_current{ 1, 1 };

	char            advance ();
	char            peek () const;
	bool            isAtEnd () const;
	void            addToken (TokenType type, TokenSpan span,
	                          const std::string &lexeme);
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
