// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloth {

class SourceFile;

// Lines and UTF-8 byte columns are one-based. The end position is exclusive.
struct TokenSpan {
	size_t                            startLine;
	size_t                            startCol;
	size_t                            endLine;
	size_t                            endCol;
	std::shared_ptr<const SourceFile> source = nullptr;

	[[nodiscard]] std::string to_string () const {
		return std::to_string(startLine) + ":" + std::to_string(startCol) + "-" + std::to_string(endLine) + ":" + std::to_string(endCol);
	}

	static TokenSpan span_through (const TokenSpan &start, const TokenSpan &end) {
		return { start.startLine, start.startCol, end.endLine, end.endCol, start.source == end.source ? start.source : nullptr };
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

enum Keyword {
	kMod,
	kDef,
	kTypeDef,
	kUsing,
	kSelf,
	kReturn,
	kConst,
	kDefault,
	kInterop,

	kNew,
	kDelete,
	kFinal,
	kOpt,

	kClass,
	kStruct,
	kEnum,
	kInterface,

	kMt,
	kOverride,
	kVirtual,
	kOperator,
	kAuto,
	kRange,
	kFn,
	kPublic,

	kBoolean,
	kString,
	kFloat,
	kInteger,
	kCharacter,
	kVoid,
	kTypeinfo,
	kState,
	kTrue,
	kFalse,
	kNull,
	kUnsigned,
	kUsize,
	kIsize,

	kInit,

	kIf,
	kElse,
	kIs,

	kFor,
	kWhile,
	kDo,
	kContinue,
	kBreak,

	kIntrinsic,
	kError

};

enum Operator {
	oPlus,
	oPlusPlus,
	oPlusEqual,

	oMinus,
	oMinusMinus,
	oMinusEqual,

	oMul,
	oMulMul,
	oMulEqual,
	oDiv,
	oDivEqual,
	oMod,
	oModEqual,

	oAssign,
	oFinalAssign,

	oEqual,
	oIdentity,
	oNotEqual,
	oNot,
	oAnd,
	oAmpersand,
	oOr,

	oGreater,
	oLess,
	oGreaterEqual,
	oLessEqual,

	oPointerRight,
	oPointerLeft,

	oQuestion,
	oSafeDot,
	oElvis,
	oRange,
	oRangeInclusive,

	oParenOpen,
	oParenClose,
	oBraceOpen,
	oBraceClose,
	oBracketOpen,
	oBracketClose,

	oComma,
	oDot,
	oColon,
	oBacktick,
	oQuotation,
	oDoubleQuotation,
	oTilde,
	oPound,
	oColonColon,
	oSemicolon,
	oUnderscore,

	oIndexRead,
	oIndexValidate,
	oIndexWrite,
	oForeach,

	oError
};

std::string_view        to_string (Keyword keyword);
std::string             to_stringmulti (const std::vector<Keyword> &keywords);
bool                    isReserved (Keyword keyword);
std::string_view        to_string (Operator op);
std::string_view        operator_name (Operator op);
std::optional<Operator> fromCharacter (std::string_view spelling);
bool                    isTokenized (Operator op);
bool                    isNumericPrefix (Operator op);
bool                    isUpdate (Operator op);
bool                    isEquality (Operator op);
bool                    isComparison (Operator op);
Operator                compound_assignment_base (std::string_view spelling);
std::string_view        to_string (TokenType type);
std::string_view        rawStringDelimiter ();

class Token {
	TokenSpan                          t_span;
	TokenType                          t_type;
	Operator                           o_operator = oError;
	std::string                        s_lexeme;
	std::shared_ptr<const std::string> p_normalizedName;

    public:
	Token (std::string lexeme, TokenType type, TokenSpan span);

	Token (std::string lexeme, const Operator op, TokenSpan span) :
	        t_span(std::move(span)),
	        t_type(TokenType::Operator),
	        o_operator(op),
	        s_lexeme(std::move(lexeme)) {
	}

	Token (const Token &other) = default;

	[[nodiscard]] const std::string &get_lexeme () const {
		return s_lexeme;
	}

	// Name comparisons use NFC. The lexeme and span keep the spelling as written.
	[[nodiscard]] const std::string &get_name_key () const {
		return p_normalizedName ? *p_normalizedName : s_lexeme;
	}

	[[nodiscard]] TokenSpan get_span () const {
		return t_span;
	}

	[[nodiscard]] TokenType get_type () const {
		return t_type;
	}

	[[nodiscard]] Operator get_operator () const {
		if (o_operator == oError && t_type == TokenType::Operator) {
			return fromCharacter(s_lexeme).value_or(oError);
		}
		return o_operator;
	}

	[[nodiscard]] std::string_view type_name () const {
		return o_operator == oError ? cloth::to_string(t_type) : operator_name(o_operator);
	}

	[[nodiscard]] std::string to_string () const {
		return "{ l: " + s_lexeme + " t: " + std::string(type_name()) + " s: " + t_span.to_string() + " }";
	}

	// Allows printing directly with std::cout << token
	friend std::ostream &operator<<(std::ostream &os, const Token &token) {
		return os << token.to_string();
	}
};

}
