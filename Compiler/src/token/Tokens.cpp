#include <token/Tokens.hpp>

namespace Tokens
{

std::string_view to_string(Keyword keyword)
{
	switch (keyword) {
	case k_MOD:
		return "mod";
	case k_DEF:
		return "def";
	case k_TYPEDEF:
		return "typedef";
	case k_USING:
		return "using";
	case k_SELF:
		return "self";
	case k_RETURN:
		return "return";
	case k_CONST:
		return "const";
	case k_DEFAULT:
		return "default";
	case k_INTEROP:
		return "interop";
	case k_NEW:
		return "new";
	case k_DELETE:
		return "delete";
	case k_FINAL:
		return "final";
	case k_OPT:
		return "opt";
	case k_CLASS:
		return "class";
	case k_STRUCT:
		return "struct";
	case k_ENUM:
		return "enum";
	case k_INTERFACE:
		return "interface";
	case k_MT:
		return "mt";
	case k_OVERRIDE:
		return "override";
	case k_VIRTUAL:
		return "virtual";
	case k_OPERATOR:
		return "operator";
	case k_AUTO:
		return "auto";
	case k_FN:
		return "fn";
	case k_PUBLIC:
		return "public";
	case k_PRIVATE:
		return "private";
	case k_BOOLEAN:
		return "boolean";
	case k_STRING:
		return "string";
	case k_FLOAT:
		return "float";
	case k_INTEGER:
		return "int";
	case k_CHARACTER:
		return "char";
	case k_VOID:
		return "void";
	case k_TYPEINFO:
		return "typeinfo";
	case k_STATE:
		return "state";
	case k_TRUE:
		return "true";
	case k_FALSE:
		return "false";
	case k_NULL:
		return "null";
	case k_UNSIGNED:
		return "unsigned";
	case k_USIZE:
		return "usize";
	case k_ISIZE:
		return "isize";
	case k_INIT:
		return "init";
	case k_IF:
		return "if";
	case k_ELSE:
		return "else";
	case k_IS:
		return "is";
	case k_FOR:
		return "for";
	case k_WHILE:
		return "while";
	case k_DO:
		return "do";
	case k_CONTINUE:
		return "continue";
	case k_BREAK:
		return "break";
	default:
		return "error";
	}
	return "";
}

namespace
{

struct OperatorInfo {
	std::string_view spelling;
	std::string_view name;
};

OperatorInfo operator_info(Operator op)
{
	switch (op) {
	case o_PLUS:
		return { "+", "PLUS" };
	case o_PLUS_PLUS:
		return { "++", "PLUS_PLUS" };
	case o_PLUS_EQUAL:
		return { "+=", "PLUS_EQUAL" };
	case o_MINUS:
		return { "-", "MINUS" };
	case o_MINUS_MINUS:
		return { "--", "MINUS_MINUS" };
	case o_MINUS_EQUAL:
		return { "-=", "MINUS_EQUAL" };
	case o_MULTIPLY:
		return { "*", "MULTIPLY" };
	case o_MUL_MUL:
		return { "**", "MUL_MUL" };
	case o_MUL_EQUAL:
		return { "*=", "MUL_EQUAL" };
	case o_DIVIDE:
		return { "/", "DIVIDE" };
	case o_DIV_EQUAL:
		return { "/=", "DIV_EQUAL" };
	case o_MODULO:
		return { "%", "MODULO" };
	case o_MOD_EQUAL:
		return { "%=", "MOD_EQUAL" };
	case o_ASSIGN:
		return { "=", "ASSIGN" };
	case o_FINAL_ASSIGN:
		return { ":=", "FINAL_ASSIGN" };
	case o_EQUAL:
		return { "==", "EQUAL" };
	case o_IDENTITY:
		return { "===", "IDENTITY" };
	case o_NOT_EQUAL:
		return { "!=", "NOT_EQUAL" };
	case o_NOT:
		return { "!", "NOT" };
	case o_AND:
		return { "&&", "AND" };
	case o_AMPERSAND:
		return { "&", "AMPERSAND" };
	case o_OR:
		return { "||", "OR" };
	case o_GREATER:
		return { ">", "GREATER" };
	case o_LESS:
		return { "<", "LESS" };
	case o_GREATER_EQUAL:
		return { ">=", "GREATER_EQUAL" };
	case o_LESS_EQUAL:
		return { "<=", "LESS_EQUAL" };
	case o_POINTER_RIGHT:
		return { "->", "POINTER_RIGHT" };
	case o_POINTER_LEFT:
		return { "<-", "POINTER_LEFT" };
	case o_QUESTION:
		return { "?", "QUESTION" };
	case o_SAFE_DOT:
		return { "?.", "SAFE_DOT" };
	case o_ELVIS:
		return { "?:", "ELVIS" };
	case o_RANGE:
		return { "..", "RANGE" };
	case o_RANGE_INCLUSIVE:
		return { "..=", "RANGE_INCLUSIVE" };
	case o_PAREN_OPEN:
		return { "(", "PAREN_OPEN" };
	case o_PAREN_CLOSE:
		return { ")", "PAREN_CLOSE" };
	case o_BRACE_OPEN:
		return { "{", "BRACE_OPEN" };
	case o_BRACE_CLOSE:
		return { "}", "BRACE_CLOSE" };
	case o_BRACKET_OPEN:
		return { "[", "BRACKET_OPEN" };
	case o_BRACKET_CLOSE:
		return { "]", "BRACKET_CLOSE" };
	case o_COMMA:
		return { ",", "COMMA" };
	case o_DOT:
		return { ".", "DOT" };
	case o_COLON:
		return { ":", "COLON" };
	case o_BACKTICK:
		return { "`", "BACKTICK" };
	case o_QUOTATION:
		return { "'", "QUOTATION" };
	case o_DOUBLE_QUOTATION:
		return { "\"", "DOUBLE_QUOTATION" };
	case o_TILDE:
		return { "~", "TILDE" };
	case o_POUND:
		return { "#", "POUND" };
	case o_COLON_COLON:
		return { "::", "COLON_COLON" };
	case o_SEMICOLON:
		return { ";", "SEMICOLON" };
	case o_UNDERSCORE:
		return { "_", "UNDERSCORE" };
	case o_ERROR:
		break;
	}
	return { "error", "ERROR" };
}

} // namespace

std::string_view to_string(Operator op)
{
	return operator_info(op).spelling;
}

std::string_view operator_name(Operator op)
{
	return operator_info(op).name;
}

}
