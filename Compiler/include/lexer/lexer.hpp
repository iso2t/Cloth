// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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
	explicit Lexer (std::string src) : Lexer(SourceFile{ "<input>", std::move(src) }) {
	}

	explicit Lexer (SourceFile source) : p_source(std::make_shared<const SourceFile>(std::move(source))) {
	}

	explicit Lexer (const char *src) : Lexer(std::string(src)) {
	}

	Lexer (const char *src, const size_t length) : Lexer(std::string(src, length)) {
	}

	Lexer (const SourceFile &source, size_t offset) : Lexer(SourceFile{ source.get_path(), source.get_contents().substr(offset), source.get_extension() }) {
	}

	~Lexer () = default;

	std::vector<Token> scanTokens ();
	void               scan ();

	[[nodiscard]] std::vector<Token> get_tokens () const {
		return v_tokens;
	}

    private:
	std::shared_ptr<const SourceFile> p_source;
	std::vector<Token>                v_tokens;
	int                               i_start   = 0;
	int                               i_current = 0;

	Position p_start{ 1, 1 };
	Position p_current{ 1, 1 };

	[[nodiscard]] const std::string &source_text () const noexcept {
		return p_source->get_contents();
	}

	char               advance ();
	void               advanceBytes (std::size_t length);
	[[nodiscard]] char peek () const;
	[[nodiscard]] bool isAtEnd () const;
	void               addToken (TokenType type, const TokenSpan &span, const std::string &lexeme);
	TokenSpan          span () const;
	TokenSpan          cursorSpan () const;
	Operator           matchOperator ();
	void               scanToken ();
	bool               skipComments ();
	void               number ();
	void               consumeNumericDigits (int radix);
	void               identifier ();
	void               consumeIdentifierParts ();
	void               interpolatedString ();
	void               rawString ();
	void               advanceStringScalar ();
	void               string ();
	void               character ();

	[[noreturn]] void invalidNumber (std::string_view reason);
	[[noreturn]] void invalidNumericSeparator () const;
};
}
