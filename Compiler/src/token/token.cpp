// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <token/token.hpp>
#include <util/unicode_identifier.hpp>

namespace cloth {

Token::Token (std::string lexeme, TokenType type, TokenSpan span) : t_span(std::move(span)), t_type(type), s_lexeme(std::move(lexeme)) {
	if (type == TokenType::Identifier && std::ranges::any_of(s_lexeme, [] (unsigned char byte) { return byte >= 0x80; })) {
		std::string s_key = normalizeIdentifier(s_lexeme);
		if (s_key != s_lexeme) p_normalizedName = std::make_shared<const std::string>(std::move(s_key));
	}
}

std::string_view toString (const TokenType type) {
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

std::string_view toString (const Keyword keyword) {
	switch (keyword) {
	case KMod:
		return "mod";
	case KDef:
		return "def";
	case KTypeDef:
		return "typedef";
	case KUsing:
		return "using";
	case KSelf:
		return "self";
	case KReturn:
		return "return";
	case KConst:
		return "const";
	case KDefault:
		return "default";
	case KInterop:
		return "interop";
	case KNew:
		return "new";
	case KDelete:
		return "delete";
	case KFinal:
		return "final";
	case KOpt:
		return "opt";
	case KClass:
		return "class";
	case KStruct:
		return "struct";
	case KEnum:
		return "enum";
	case KInterface:
		return "interface";
	case KMt:
		return "mt";
	case KOverride:
		return "override";
	case KVirtual:
		return "virtual";
	case KOperator:
		return "operator";
	case KRange:
		return "range";
	case KAuto:
		return "auto";
	case KFn:
		return "fn";
	case KPublic:
		return "pub";
	case KBoolean:
		return "bool";
	case KString:
		return "string";
	case KFloat:
		return "float";
	case KInteger:
		return "int";
	case KCharacter:
		return "char";
	case KVoid:
		return "void";
	case KTypeinfo:
		return "typeinfo";
	case KState:
		return "state";
	case KTrue:
		return "true";
	case KFalse:
		return "false";
	case KNull:
		return "null";
	case KUnsigned:
		return "unsigned";
	case KUsize:
		return "usize";
	case KIsize:
		return "isize";
	case KInit:
		return "init";
	case KIf:
		return "if";
	case KElse:
		return "else";
	case KIs:
		return "is";
	case KFor:
		return "for";
	case KWhile:
		return "while";
	case KDo:
		return "do";
	case KContinue:
		return "continue";
	case KBreak:
		return "break";
	case KIntrinsic:
		return "intrinsic";
	default:
		return "error";
	}
	return "";
}

std::string toStringmulti (const std::vector<Keyword> &keywords) {
	if (keywords.empty()) {
		return {};
	}

	std::size_t st_total = keywords.size() - 1;
	for (const Keyword k_keyword : keywords) {
		st_total += toString(k_keyword).size();
	}

	std::string s_result;
	s_result.reserve(st_total);
	for (const Keyword k_keyword : keywords) {
		if (!s_result.empty()) {
			s_result += ' ';
		}
		s_result += toString(k_keyword);
	}

	return s_result;
}

bool isReserved (Keyword keyword) {
	return keyword != KRange && keyword != KAuto && keyword != KIntrinsic && keyword != KError;
}

namespace {

struct OperatorInfo {
	std::string_view spelling;
	std::string_view name;
	bool             tokenized = true;
};

constexpr std::pair<Operator, Operator> COMPOUND_ASSIGNMENTS[] = {
	{ OPlusEqual, OPlus }, { OMinusEqual, OMinus }, { OMulEqual, OMul }, { ODivEqual, ODiv }, { OModEqual, OMod },
};

OperatorInfo operatorInfo (Operator op) {
	switch (op) {
	case OPlus:
		return { .spelling="+", .name="PLUS" };
	case OPlusPlus:
		return { .spelling="++", .name="PLUS_PLUS" };
	case OPlusEqual:
		return { .spelling="+=", .name="PLUS_EQUAL" };
	case OMinus:
		return { .spelling="-", .name="MINUS" };
	case OMinusMinus:
		return { .spelling="--", .name="MINUS_MINUS" };
	case OMinusEqual:
		return { .spelling="-=", .name="MINUS_EQUAL" };
	case OMul:
		return { .spelling="*", .name="MULTIPLY" };
	case OMulMul:
		return { .spelling="**", .name="MUL_MUL" };
	case OMulEqual:
		return { .spelling="*=", .name="MUL_EQUAL" };
	case ODiv:
		return { .spelling="/", .name="DIVIDE" };
	case ODivEqual:
		return { .spelling="/=", .name="DIV_EQUAL" };
	case OMod:
		return { .spelling="%", .name="MODULO" };
	case OModEqual:
		return { .spelling="%=", .name="MOD_EQUAL" };
	case OAssign:
		return { .spelling="=", .name="ASSIGN" };
	case OFinalAssign:
		return { .spelling=":=", .name="FINAL_ASSIGN" };
	case OEqual:
		return { .spelling="==", .name="EQUAL" };
	case OIdentity:
		return { .spelling="===", .name="IDENTITY" };
	case ONotEqual:
		return { .spelling="!=", .name="NOT_EQUAL" };
	case ONot:
		return { .spelling="!", .name="NOT" };
	case OAnd:
		return { .spelling="&&", .name="AND" };
	case OAmpersand:
		return { .spelling="&", .name="AMPERSAND" };
	case OOr:
		return { .spelling="||", .name="OR" };
	case OGreater:
		return { .spelling=">", .name="GREATER" };
	case OLess:
		return { .spelling="<", .name="LESS" };
	case OGreaterEqual:
		return { .spelling=">=", .name="GREATER_EQUAL" };
	case OLessEqual:
		return { .spelling="<=", .name="LESS_EQUAL" };
	case OPointerRight:
		return { .spelling="->", .name="POINTER_RIGHT" };
	case OPointerLeft:
		return { .spelling="<-", .name="POINTER_LEFT" };
	case OQuestion:
		return { .spelling="?", .name="QUESTION" };
	case OSafeDot:
		return { .spelling="?.", .name="SAFE_DOT" };
	case OElvis:
		return { .spelling="?:", .name="ELVIS" };
	case ORange:
		return { .spelling="..", .name="RANGE" };
	case ORangeInclusive:
		return { .spelling="..=", .name="RANGE_INCLUSIVE" };
	case OParenOpen:
		return { .spelling="(", .name="PAREN_OPEN" };
	case OParenClose:
		return { .spelling=")", .name="PAREN_CLOSE" };
	case OBraceOpen:
		return { .spelling="{", .name="BRACE_OPEN" };
	case OBraceClose:
		return { .spelling="}", .name="BRACE_CLOSE" };
	case OBracketOpen:
		return { .spelling="[", .name="BRACKET_OPEN" };
	case OBracketClose:
		return { .spelling="]", .name="BRACKET_CLOSE" };
	case OComma:
		return { .spelling=",", .name="COMMA" };
	case ODot:
		return { .spelling=".", .name="DOT" };
	case OColon:
		return { .spelling=":", .name="COLON" };
	case OBacktick:
		return { .spelling="`", .name="BACKTICK" };
	case OQuotation:
		return { .spelling="'", .name="QUOTATION" };
	case ODoubleQuotation:
		return { .spelling="\"", .name="DOUBLE_QUOTATION" };
	case OTilde:
		return { .spelling="~", .name="TILDE" };
	case OPound:
		return { .spelling="#", .name="POUND" };
	case OColonColon:
		return { .spelling="::", .name="COLON_COLON" };
	case OSemicolon:
		return { .spelling=";", .name="SEMICOLON" };
	case OUnderscore:
		return { .spelling="_", .name="UNDERSCORE" };
	case OIndexRead:
		return { .spelling="[]", .name="INDEX_READ", .tokenized=false };
	case OIndexValidate:
		return { .spelling="[]?", .name="INDEX_VALIDATE", .tokenized=false };
	case OIndexWrite:
		return { .spelling="[]=", .name="INDEX_WRITE", .tokenized=false };
	case OForeach:
		return { .spelling="foreach", .name="FOREACH", .tokenized=false };
	case OError:
		break;
	}
	return { .spelling = "error", .name = "ERROR", .tokenized = false };
}

}

std::string_view toString (Operator op) {
	return operatorInfo(op).spelling;
}

std::string_view rawStringDelimiter () {
	static const std::string S_DELIMITER = std::format("{0}{0}{0}", toString(ODoubleQuotation));
	return S_DELIMITER;
}

std::optional<Operator> fromCharacter (const std::string_view spelling) {
	static const std::unordered_map<std::string_view, Operator> M_OPERATORS = [] {
		std::unordered_map<std::string_view, Operator> m_operators;
		for (int i_value = OPlus; i_value < OError; ++i_value) {
			const auto o_operator = static_cast<Operator>(i_value);
			m_operators.emplace(toString(o_operator), o_operator);
		}
		return m_operators;
	}();

	const auto it_operator = M_OPERATORS.find(spelling);
	return it_operator == M_OPERATORS.end() ? std::nullopt : std::make_optional(it_operator->second);
}

bool isTokenized (const Operator op) {
	return operatorInfo(op).tokenized;
}

bool isNumericPrefix (Operator op) {
	return op == OPlus || op == OMinus || op == OTilde;
}

bool isUpdate (Operator op) {
	return op == OPlusPlus || op == OMinusMinus ||
	       std::ranges::any_of(COMPOUND_ASSIGNMENTS, [op] (const std::pair<Operator, Operator> &assignment) { return assignment.first == op; });
}

bool isEquality (Operator op) {
	return op == OEqual || op == ONotEqual || op == OIdentity;
}

bool isComparison (Operator op) {
	return isEquality(op) || op == OLess || op == OGreater || op == OLessEqual || op == OGreaterEqual;
}

Operator compoundAssignmentBase (std::string_view spelling) {
	for (const std::pair<Operator, Operator> &oo_assignment : COMPOUND_ASSIGNMENTS) {
		if (spelling == toString(oo_assignment.first)) {
			return oo_assignment.second;
		}
	}
	throw std::logic_error("Not a compound assignment: " + std::string(spelling));
}

std::string_view operatorName (Operator op) {
	return operatorInfo(op).name;
}

}
