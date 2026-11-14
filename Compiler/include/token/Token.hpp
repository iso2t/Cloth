#pragma once

#include <token/Tokens.hpp>

#include <ostream>
#include <string>
#include <string_view>
#include <utility>

struct TokenSpan {
	size_t startLine;
	size_t startCol;
	size_t endLine;
	size_t endCol;

	std::string to_string() const {
		return std::to_string(startLine) + ":" + std::to_string(startCol) + "-" + std::to_string(endLine) + ":" + std::to_string(endCol);
	}
};

enum TokenType {
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

std::string_view to_string(TokenType type);

class Token {
    private:
	TokenSpan m_span;
	TokenType m_type;
	Tokens::Operator m_operator = Tokens::o_ERROR;
	std::string m_lexeme;

    public:
	Token(std::string lexeme, TokenType type, TokenSpan span)
		: m_span(span)
		, m_type(type)
		, m_lexeme(std::move(lexeme)) {
	}

	Token(std::string lexeme, Tokens::Operator op, TokenSpan span)
		: m_span(span)
		, m_type(TokenType::Operator)
		, m_operator(op)
		, m_lexeme(std::move(lexeme)) {
	}

	Token(char *lexeme, TokenType type, TokenSpan span)
		: m_span(span)
		, m_type(type)
		, m_lexeme(lexeme, sizeof(lexeme)) {
	}

	Token(const Token &other) = default;

	const std::string &get_lexeme() const {
		return m_lexeme;
	}

	TokenSpan get_span() const {
		return m_span;
	}

	TokenType get_type() const {
		return m_type;
	}

	std::string_view type_name() const {
		return m_operator == Tokens::o_ERROR ? ::to_string(m_type) : Tokens::operator_name(m_operator);
	}

	std::string to_string() const {
		return "{ l: " + m_lexeme + " t: " + std::string(type_name()) + " s: " + m_span.to_string() + " }";
	}

	// Allows printing directly with std::cout << token
	friend std::ostream &operator<<(std::ostream &os, const Token &token) {
		return os << token.to_string();
	}
};
