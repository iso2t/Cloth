#pragma once

#include <file/source_file.hpp>
#include <token/Token.hpp>

#include <vector>

namespace cloth {
struct Position {
	size_t line;
	size_t column;
};

class Lexer {
    public:
	Lexer(const std::string &src)
		: source(src) {
	}

	Lexer(const SourceFile &co)
		: source(co.contents()) {
	}

	Lexer(const char *src)
		: source(src) {
	}

	Lexer(const char *src, size_t length)
		: source(std::string(src, length)) {
	}

	Lexer(const SourceFile &co, size_t length)
		: source(std::string(co.contents(), length)) {
	}

	~Lexer() = default;

	std::vector<Token> scanTokens();
	void scan();

	std::vector<Token> get_tokens() const {
		return tokens;
	}

    private:
	std::string source;
	std::vector<Token> tokens;
	int start = 0;
	int current = 0;

	Position p_start{ 1, 1 };
	Position p_current{ 1, 1 };

	char advance();
	char peek() const;
	bool isAtEnd() const;
	void addToken(TokenType type, TokenSpan span, const std::string &lexeme);
	TokenSpan span();
	TokenSpan cursorSpan();
	cloth::Operator matchOperator();
	void scanToken();
	bool skipComments();
	void number();
	void identifier();
	void interpolatedString();
	void rawString();
	void advanceStringScalar();
	void string();
	void character();
};
}
