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

	[[nodiscard]] std::string toString () const {
		return std::to_string(startLine) + ":" + std::to_string(startCol) + "-" + std::to_string(endLine) + ":" + std::to_string(endCol);
	}

	static TokenSpan spanThrough (const TokenSpan &start, const TokenSpan &end) {
		return { .startLine=start.startLine, .startCol=start.startCol, .endLine=end.endLine, .endCol=end.endCol, .source=start.source == end.source ? start.source : nullptr };
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
	KMod,
	KDef,
	KTypeDef,
	KUsing,
	KSelf,
	KReturn,
	KConst,
	KDefault,
	KInterop,

	KNew,
	KDelete,
	KFinal,
	KOpt,

	KClass,
	KStruct,
	KEnum,
	KInterface,

	KMt,
	KOverride,
	KVirtual,
	KOperator,
	KAuto,
	KRange,
	KFn,
	KPublic,

	KBoolean,
	KString,
	KFloat,
	KInteger,
	KCharacter,
	KVoid,
	KTypeinfo,
	KState,
	KTrue,
	KFalse,
	KNull,
	KUnsigned,
	KUsize,
	KIsize,

	KInit,

	KIf,
	KElse,
	KIs,

	KFor,
	KWhile,
	KDo,
	KContinue,
	KBreak,

	KIntrinsic,
	KError

};

enum Operator {
	OPlus,
	OPlusPlus,
	OPlusEqual,

	OMinus,
	OMinusMinus,
	OMinusEqual,

	OMul,
	OMulMul,
	OMulEqual,
	ODiv,
	ODivEqual,
	OMod,
	OModEqual,

	OAssign,
	OFinalAssign,

	OEqual,
	OIdentity,
	ONotEqual,
	ONot,
	OAnd,
	OAmpersand,
	OOr,

	OGreater,
	OLess,
	OGreaterEqual,
	OLessEqual,

	OPointerRight,
	OPointerLeft,

	OQuestion,
	OSafeDot,
	OElvis,
	ORange,
	ORangeInclusive,

	OParenOpen,
	OParenClose,
	OBraceOpen,
	OBraceClose,
	OBracketOpen,
	OBracketClose,

	OComma,
	ODot,
	OColon,
	OBacktick,
	OQuotation,
	ODoubleQuotation,
	OTilde,
	OPound,
	OColonColon,
	OSemicolon,
	OUnderscore,

	OIndexRead,
	OIndexValidate,
	OIndexWrite,
	OForeach,

	OError
};

std::string_view        toString (Keyword keyword);
std::string             toStringmulti (const std::vector<Keyword> &keywords);
bool                    isReserved (Keyword keyword);
std::string_view        toString (Operator op);
std::string_view        operatorName (Operator op);
std::optional<Operator> fromCharacter (std::string_view spelling);
bool                    isTokenized (Operator op);
bool                    isNumericPrefix (Operator op);
bool                    isUpdate (Operator op);
bool                    isEquality (Operator op);
bool                    isComparison (Operator op);
Operator                compoundAssignmentBase (std::string_view spelling);
std::string_view        toString (TokenType type);
std::string_view        rawStringDelimiter ();

class Token {
	TokenSpan                          t_span;
	TokenType                          t_type;
	Operator                           o_operator = OError;
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

	[[nodiscard]] const std::string &getLexeme () const {
		return s_lexeme;
	}

	// Name comparisons use NFC. The lexeme and span keep the spelling as written.
	[[nodiscard]] const std::string &getNameKey () const {
		return p_normalizedName ? *p_normalizedName : s_lexeme;
	}

	[[nodiscard]] TokenSpan getSpan () const {
		return t_span;
	}

	[[nodiscard]] TokenType getType () const {
		return t_type;
	}

	[[nodiscard]] Operator getOperator () const {
		if (o_operator == OError && t_type == TokenType::Operator) {
			return fromCharacter(s_lexeme).value_or(OError);
		}
		return o_operator;
	}

	[[nodiscard]] std::string_view typeName () const {
		return o_operator == OError ? cloth::toString(t_type) : operatorName(o_operator);
	}

	[[nodiscard]] std::string toString () const {
		return "{ l: " + s_lexeme + " t: " + std::string(typeName()) + " s: " + t_span.toString() + " }";
	}

	// Allows printing directly with std::cout << token
	friend std::ostream &operator<<(std::ostream &os, const Token &token) {
		return os << token.toString();
	}
};

}
