// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace cloth {

struct TokenSpan {
	size_t startLine;
	size_t startCol;
	size_t endLine;
	size_t endCol;

	std::string to_string () const {
		return std::to_string(startLine) + ":" + std::to_string(startCol) + "-" + std::to_string(endLine) + ":" + std::to_string(endCol);
	}
};

enum class TokenType {
	EndOfFile,
	Unknown,
	NewLine,

	Identifier,
	Keyword,
	Operator,

	FloatingPointLiteral,
	IntegerLiteral,
	StringLiteral,
	RawStringLiteral,
	CharacterLiteral,
	InterpolationStart,
	StringSegment,
	InterpolationOpen,
	InterpolationClose,
	InterpolationEnd
};

std::string_view to_string (TokenType type);

enum Keyword {
	k_MOD,
	k_DEF,
	k_TYPEDEF,
	k_USING,
	k_SELF,
	k_RETURN,
	k_CONST,
	k_DEFAULT,
	k_INTEROP,

	k_NEW,
	k_DELETE,
	k_FINAL,
	k_OPT,

	k_CLASS,
	k_STRUCT,
	k_ENUM,
	k_INTERFACE,

	k_MT,
	k_OVERRIDE,
	k_VIRTUAL,
	k_OPERATOR,
	k_AUTO,
	k_FN,
	k_PUBLIC,
	k_PRIVATE,

	k_BOOLEAN,
	k_STRING,
	k_FLOAT,
	k_INTEGER,
	k_CHARACTER,
	k_VOID,
	k_TYPEINFO,
	k_STATE,
	k_TRUE,
	k_FALSE,
	k_NULL,
	k_UNSIGNED,
	k_USIZE,
	k_ISIZE,

	k_INIT,

	k_IF,
	k_ELSE,
	k_IS,

	k_FOR,
	k_WHILE,
	k_DO,
	k_CONTINUE,
	k_BREAK,

	k_ERROR

};

enum Operator {
	o_PLUS,
	o_PLUS_PLUS,
	o_PLUS_EQUAL,

	o_MINUS,
	o_MINUS_MINUS,
	o_MINUS_EQUAL,

	o_MULTIPLY,
	o_MUL_MUL,
	o_MUL_EQUAL,
	o_DIVIDE,
	o_DIV_EQUAL,
	o_MODULO,
	o_MOD_EQUAL,

	o_ASSIGN,
	o_FINAL_ASSIGN,

	o_EQUAL,
	o_IDENTITY,
	o_NOT_EQUAL,
	o_NOT,
	o_AND,
	o_AMPERSAND,
	o_OR,

	o_GREATER,
	o_LESS,
	o_GREATER_EQUAL,
	o_LESS_EQUAL,

	o_POINTER_RIGHT,
	o_POINTER_LEFT,

	o_QUESTION,
	o_SAFE_DOT,
	o_ELVIS,
	o_RANGE,
	o_RANGE_INCLUSIVE,

	o_PAREN_OPEN,
	o_PAREN_CLOSE,
	o_BRACE_OPEN,
	o_BRACE_CLOSE,
	o_BRACKET_OPEN,
	o_BRACKET_CLOSE,

	o_COMMA,
	o_DOT,
	o_COLON,
	o_BACKTICK,
	o_QUOTATION,
	o_DOUBLE_QUOTATION,
	o_TILDE,
	o_POUND,
	o_COLON_COLON,
	o_SEMICOLON,
	o_UNDERSCORE,

	o_ERROR
};

std::string_view to_string (Keyword keyword);
std::string_view to_string (Operator op);
std::string_view operator_name (Operator op);

class Token {
    private:
	TokenSpan       t_span;
	TokenType       t_type;
	cloth::Operator o_operator = cloth::Operator::o_ERROR;
	std::string     s_lexeme;

    public:
	Token (std::string lexeme, TokenType type, TokenSpan span) : t_span(span), t_type(type), s_lexeme(std::move(lexeme)) {
	}

	Token (std::string lexeme, cloth::Operator op, TokenSpan span) :
	        t_span(span),
	        t_type(TokenType::Operator),
	        o_operator(op),
	        s_lexeme(std::move(lexeme)) {
	}

	Token (char *lexeme, TokenType type, TokenSpan span) : t_span(span), t_type(type), s_lexeme(lexeme, sizeof(lexeme)) {
	}

	Token (const Token &other) = default;

	const std::string &get_lexeme () const {
		return s_lexeme;
	}

	TokenSpan get_span () const {
		return t_span;
	}

	TokenType get_type () const {
		return t_type;
	}

	std::string_view type_name () const {
		return o_operator == cloth::o_ERROR ? cloth::to_string(t_type) : cloth::operator_name(o_operator);
	}

	std::string to_string () const {
		return "{ l: " + s_lexeme + " t: " + std::string(type_name()) + " s: " + t_span.to_string() + " }";
	}

	// Allows printing directly with std::cout << token
	friend std::ostream &operator<<(std::ostream &os, const Token &token) {
		return os << token.to_string();
	}
};

}
