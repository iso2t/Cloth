// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <token/token.hpp>

namespace cloth {

std::string_view to_string (TokenType type) {
	switch (type) {
	case TokenType::EndOfFile:
		return "EndOfFile";
	case TokenType::Unknown:
		return "Unknown";
	case TokenType::NewLine:
		return "NewLine";
	case TokenType::Identifier:
		return "Identifier";
	case TokenType::Keyword:
		return "Keyword";
	case TokenType::Operator:
		return "Operator";
	case TokenType::FloatingPointLiteral:
		return "FloatingPointLiteral";
	case TokenType::IntegerLiteral:
		return "IntegerLiteral";
	case TokenType::StringLiteral:
		return "StringLiteral";
	case TokenType::RawStringLiteral:
		return "RawStringLiteral";
	case TokenType::CharacterLiteral:
		return "CharacterLiteral";
	case TokenType::InterpolationStart:
		return "InterpolationStart";
	case TokenType::StringSegment:
		return "StringSegment";
	case TokenType::InterpolationOpen:
		return "InterpolationOpen";
	case TokenType::InterpolationClose:
		return "InterpolationClose";
	case TokenType::InterpolationEnd:
		return "InterpolationEnd";
	}
	return "Invalid";
}

std::string_view to_string (Keyword keyword) {
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

namespace {

struct OperatorInfo {
	std::string_view spelling;
	std::string_view name;
	bool             tokenized = true;
};

constexpr std::pair<Operator, Operator> COMPOUND_ASSIGNMENTS[] = {
	{ o_PLUS_EQUAL, o_PLUS }, { o_MINUS_EQUAL, o_MINUS }, { o_MUL_EQUAL, o_MULTIPLY }, { o_DIV_EQUAL, o_DIVIDE }, { o_MOD_EQUAL, o_MODULO },
};

OperatorInfo operator_info (Operator op) {
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
	case o_INDEX_READ:
		return { "[]", "INDEX_READ", false };
	case o_INDEX_VALIDATE:
		return { "[]?", "INDEX_VALIDATE", false };
	case o_INDEX_WRITE:
		return { "[]=", "INDEX_WRITE", false };
	case o_FOREACH:
		return { "foreach", "FOREACH", false };
	case o_ERROR:
		break;
	}
	return { "error", "ERROR", false };
}

}

std::string_view to_string (Operator op) {
	return operator_info(op).spelling;
}

std::optional<Operator> fromCharacter (std::string_view spelling) {
	static const std::unordered_map<std::string_view, Operator> M_OPERATORS = [] {
		std::unordered_map<std::string_view, Operator> m_operators;
		for (int i_value = o_PLUS; i_value < o_ERROR; ++i_value) {
			const Operator o_operator = static_cast<Operator>(i_value);
			m_operators.emplace(to_string(o_operator), o_operator);
		}
		return m_operators;
	}();

	const std::unordered_map<std::string_view, Operator>::const_iterator it_operator = M_OPERATORS.find(spelling);
	return it_operator == M_OPERATORS.end() ? std::nullopt : std::make_optional(it_operator->second);
}

bool isTokenized (Operator op) {
	return operator_info(op).tokenized;
}

bool isUpdate (Operator op) {
	return op == o_PLUS_PLUS || op == o_MINUS_MINUS ||
	       std::ranges::any_of(COMPOUND_ASSIGNMENTS, [op] (const std::pair<Operator, Operator> &assignment) { return assignment.first == op; });
}

Operator compound_assignment_base (std::string_view spelling) {
	for (const std::pair<Operator, Operator> &oo_assignment : COMPOUND_ASSIGNMENTS) {
		if (spelling == to_string(oo_assignment.first)) {
			return oo_assignment.second;
		}
	}
	throw std::logic_error("Not a compound assignment: " + std::string(spelling));
}

std::string_view operator_name (Operator op) {
	return operator_info(op).name;
}

}
