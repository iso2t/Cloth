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

	Lexer (const SourceFile &source, std::size_t offset);

	~Lexer () = default;

	std::vector<Token> scanTokens ();
	void               scan ();

	[[nodiscard]] std::vector<Token> getTokens () const {
		return v_tokens;
	}

    private:
	std::shared_ptr<const SourceFile> p_source;
	std::vector<Token>                v_tokens;
	std::size_t                       st_start   = 0;
	std::size_t                       st_current = 0;

	Position p_start{ .line=1, .column=1 };
	Position p_current{ .line=1, .column=1 };

	[[nodiscard]] const std::string &sourceText () const noexcept {
		return p_source->getContents();
	}

	char                    advance ();
	void                    advanceBytes (std::size_t length);
	bool                    consumeLineBreak ();
	[[nodiscard]] char      peek () const;
	[[nodiscard]] bool      startsWith (std::string_view spelling) const;
	[[nodiscard]] bool      isAtEnd () const;
	void                    addToken (TokenType type, const TokenSpan &span, const std::string &lexeme);
	[[nodiscard]] TokenSpan span () const;
	[[nodiscard]] TokenSpan spanFrom (Position start) const;
	Operator                matchOperator ();
	void                    scanToken ();
	bool                    skipComments ();
	void                    number ();
	void                    consumeNumericDigits (int radix);
	void                    identifier ();
	void                    consumeIdentifierParts ();
	void                    interpolatedString ();
	void                    rawString ();
	void                    advanceStringScalar ();
	void                    advanceStringCharacter (std::string_view kind);
	void                    advanceEscape (Position start, std::string_view kind);
	void                    string ();
	void                    character ();

	[[noreturn]] void invalidNumber (std::string_view reason);
	[[noreturn]] void invalidNumericSeparator () const;
};
}
