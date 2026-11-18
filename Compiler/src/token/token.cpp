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

std::string_view to_string (const TokenType type) {
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

std::string_view to_string (const Keyword keyword) {
	switch (keyword) {
	case kMod:
		return "mod";
	case kDef:
		return "def";
	case kTypeDef:
		return "typedef";
	case kUsing:
		return "using";
	case kSelf:
		return "self";
	case kReturn:
		return "return";
	case kConst:
		return "const";
	case kDefault:
		return "default";
	case kInterop:
		return "interop";
	case kNew:
		return "new";
	case kDelete:
		return "delete";
	case kFinal:
		return "final";
	case kOpt:
		return "opt";
	case kClass:
		return "class";
	case kStruct:
		return "struct";
	case kEnum:
		return "enum";
	case kInterface:
		return "interface";
	case kMt:
		return "mt";
	case kOverride:
		return "override";
	case kVirtual:
		return "virtual";
	case kOperator:
		return "operator";
	case kAuto:
		return "auto";
	case kFn:
		return "fn";
	case kPublic:
		return "pub";
	case kPrivate:
		return "private";
	case kBoolean:
		return "bool";
	case kString:
		return "string";
	case kFloat:
		return "float";
	case kInteger:
		return "int";
	case kCharacter:
		return "char";
	case kVoid:
		return "void";
	case kTypeinfo:
		return "typeinfo";
	case kState:
		return "state";
	case kTrue:
		return "true";
	case kFalse:
		return "false";
	case kNull:
		return "null";
	case kUnsigned:
		return "unsigned";
	case kUsize:
		return "usize";
	case kIsize:
		return "isize";
	case kInit:
		return "init";
	case kIf:
		return "if";
	case kElse:
		return "else";
	case kIs:
		return "is";
	case kFor:
		return "for";
	case kWhile:
		return "while";
	case kDo:
		return "do";
	case kContinue:
		return "continue";
	case kBreak:
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
	{ oPlusEqual, oPlus }, { oMinusEqual, oMinus }, { oMulEqual, oMul }, { oDivEqual, oDiv }, { oModEqual, oMod },
};

OperatorInfo operator_info (Operator op) {
	switch (op) {
	case oPlus:
		return { "+", "PLUS" };
	case oPlusPlus:
		return { "++", "PLUS_PLUS" };
	case oPlusEqual:
		return { "+=", "PLUS_EQUAL" };
	case oMinus:
		return { "-", "MINUS" };
	case oMinusMinus:
		return { "--", "MINUS_MINUS" };
	case oMinusEqual:
		return { "-=", "MINUS_EQUAL" };
	case oMul:
		return { "*", "MULTIPLY" };
	case oMulMul:
		return { "**", "MUL_MUL" };
	case oMulEqual:
		return { "*=", "MUL_EQUAL" };
	case oDiv:
		return { "/", "DIVIDE" };
	case oDivEqual:
		return { "/=", "DIV_EQUAL" };
	case oMod:
		return { "%", "MODULO" };
	case oModEqual:
		return { "%=", "MOD_EQUAL" };
	case oAssign:
		return { "=", "ASSIGN" };
	case oFinalAssign:
		return { ":=", "FINAL_ASSIGN" };
	case oEqual:
		return { "==", "EQUAL" };
	case oIdentity:
		return { "===", "IDENTITY" };
	case oNotEqual:
		return { "!=", "NOT_EQUAL" };
	case oNot:
		return { "!", "NOT" };
	case oAnd:
		return { "&&", "AND" };
	case oAmpersand:
		return { "&", "AMPERSAND" };
	case oOr:
		return { "||", "OR" };
	case oGreater:
		return { ">", "GREATER" };
	case oLess:
		return { "<", "LESS" };
	case oGreaterEqual:
		return { ">=", "GREATER_EQUAL" };
	case oLessEqual:
		return { "<=", "LESS_EQUAL" };
	case oPointerRight:
		return { "->", "POINTER_RIGHT" };
	case oPointerLeft:
		return { "<-", "POINTER_LEFT" };
	case oQuestion:
		return { "?", "QUESTION" };
	case oSafeDot:
		return { "?.", "SAFE_DOT" };
	case oElvis:
		return { "?:", "ELVIS" };
	case oRange:
		return { "..", "RANGE" };
	case oRangeInclusive:
		return { "..=", "RANGE_INCLUSIVE" };
	case oParenOpen:
		return { "(", "PAREN_OPEN" };
	case oParenClose:
		return { ")", "PAREN_CLOSE" };
	case oBraceOpen:
		return { "{", "BRACE_OPEN" };
	case oBraceClose:
		return { "}", "BRACE_CLOSE" };
	case oBracketOpen:
		return { "[", "BRACKET_OPEN" };
	case oBracketClose:
		return { "]", "BRACKET_CLOSE" };
	case oComma:
		return { ",", "COMMA" };
	case oDot:
		return { ".", "DOT" };
	case oColon:
		return { ":", "COLON" };
	case oBacktick:
		return { "`", "BACKTICK" };
	case oQuotation:
		return { "'", "QUOTATION" };
	case oDoubleQuotation:
		return { "\"", "DOUBLE_QUOTATION" };
	case oTilde:
		return { "~", "TILDE" };
	case oPound:
		return { "#", "POUND" };
	case oColonColon:
		return { "::", "COLON_COLON" };
	case oSemicolon:
		return { ";", "SEMICOLON" };
	case oUnderscore:
		return { "_", "UNDERSCORE" };
	case oIndexRead:
		return { "[]", "INDEX_READ", false };
	case oIndexValidate:
		return { "[]?", "INDEX_VALIDATE", false };
	case oIndexWrite:
		return { "[]=", "INDEX_WRITE", false };
	case oForeach:
		return { "foreach", "FOREACH", false };
	case oError:
		break;
	}
	return { .spelling = "error", .name = "ERROR", .tokenized = false };
}

}

std::string_view to_string (Operator op) {
	return operator_info(op).spelling;
}

std::optional<Operator> fromCharacter (const std::string_view spelling) {
	static const std::unordered_map<std::string_view, Operator> M_OPERATORS = [] {
		std::unordered_map<std::string_view, Operator> m_operators;
		for (int i_value = oPlus; i_value < oError; ++i_value) {
			const auto o_operator = static_cast<Operator>(i_value);
			m_operators.emplace(to_string(o_operator), o_operator);
		}
		return m_operators;
	}();

	const auto it_operator = M_OPERATORS.find(spelling);
	return it_operator == M_OPERATORS.end() ? std::nullopt : std::make_optional(it_operator->second);
}

bool isTokenized (const Operator op) {
	return operator_info(op).tokenized;
}

bool isUpdate (Operator op) {
	return op == oPlusPlus || op == oMinusMinus ||
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
