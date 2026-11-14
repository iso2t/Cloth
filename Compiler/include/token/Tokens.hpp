#pragma once

#include <string_view>

namespace Tokens
{

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

std::string_view to_string(Keyword keyword);
std::string_view to_string(Operator op);
std::string_view operator_name(Operator op);

} // namespace Tokens
