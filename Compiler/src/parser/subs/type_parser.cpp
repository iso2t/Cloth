// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/types/Generics.rst

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

constexpr std::array PRIMITIVE_TYPES{
	KInteger, KFloat, KBoolean, KString, KCharacter, KVoid, KTypeinfo, KUsize, KIsize,
};

bool isPrimitiveType (std::string_view lexeme) {
	return std::ranges::any_of(PRIMITIVE_TYPES, [lexeme] (const Keyword keyword) -> bool { return lexeme == toString(keyword); });
}

bool isTypeName (const Token &token) {
	return token.getType() == TokenType::Identifier || (token.getType() == TokenType::Keyword && isPrimitiveType(token.getLexeme()));
}

bool touches (const Token &token, const Token &next) {
	const TokenSpan ts_token = token.getSpan();
	const TokenSpan ts_next  = next.getSpan();
	return ts_token.endLine == ts_next.startLine && ts_token.endCol == ts_next.startCol;
}

}

TypeParser::ParentType TypeParser::parseParent (const std::string_view description) {
	const Token t_separator = p_parser.advance();
	p_parser.skipNewLines();
	if (const Token t_next = p_parser.peek(); t_next.getType() != TokenType::Identifier) {
		throw cde::SyntaxErrors::expected(std::string(description), t_next);
	}
	TypeReference tr_type = parse();
	if (tr_type.hasNullability()) {
		throw cde::SyntaxErrors::expected("a non-nullable parent type", p_parser.previous());
	}
	return { .separator = t_separator, .type = std::move(tr_type) };
}

std::optional<TypeParameterList> TypeParser::parseTypeParameters () {
	if (!at(OLess)) {
		return std::nullopt;
	}
	const Token t_opening = p_parser.advance();
	p_parser.skipNewLines();
	std::vector<TypeParameterDeclaration> v_parameters;
	while (true) {
		const Token t_name = p_parser.expect(TokenType::Identifier, "a type parameter name");
		p_parser.skipNewLines();
		std::vector<TypeParameterBound> v_bounds;
		if (at(OColon)) {
			v_bounds = parseBounds();
		}
		std::optional<TypeReference> opt_defaultType;
		if (at(OAssign)) {
			p_parser.advance();
			p_parser.skipNewLines();
			opt_defaultType = parse(/*inCast=*/false, /*allowArrays=*/true);
		}
		v_parameters.push_back({ .name = t_name, .bounds = std::move(v_bounds), .defaultType = std::move(opt_defaultType) });
		p_parser.skipNewLines();
		if (at(OElvis)) {
			throw cde::GenericErrors::syntax(std::format("Type defaults written with '{}'", toString(OElvis)), p_parser.peek());
		}
		if (at(OGreater)) {
			break;
		}
		p_parser.expect(OComma);
		p_parser.skipNewLines();
	}
	return TypeParameterList{ .opening = t_opening, .parameters = std::move(v_parameters), .closing = p_parser.expect(OGreater) };
}

std::vector<TypeParameterBound> TypeParser::parseBounds () {
	std::vector<TypeParameterBound> v_bounds;
	do {
		p_parser.advance();
		p_parser.skipNewLines();
		if (p_parser.atKeyword(KNew)) {
			const Token                       t_keyword    = p_parser.advance();
			std::vector<ParameterDeclaration> v_parameters = p_parser.p_functionParser.parseParameters();
			const TokenSpan                   ts_span      = TokenSpan::spanThrough(t_keyword.getSpan(), p_parser.previous().getSpan());
			v_bounds.push_back({ ConstructorBound{ ConstructorGuaranteeDeclaration(t_keyword, std::move(v_parameters), ts_span) } });
		} else {
			v_bounds.push_back({ parse(/*inCast=*/false, /*allowArrays=*/true) });
		}
		p_parser.skipNewLines();
	} while (at(OAmpersand));
	return v_bounds;
}

TypeArgumentList TypeParser::arguments () {
	const Token t_opening = p_parser.expect(OLess);
	if (i_argumentDepth >= MAX_TYPE_ARG_NESTING) {
		throw cde::GenericErrors::nesting(t_opening.getSpan(), MAX_TYPE_ARG_NESTING);
	}

	struct ArgumentDepth {
		int &depth;

		~ArgumentDepth () {
			--depth;
		}
	};

	++i_argumentDepth;
	const ArgumentDepth ad_scope{ i_argumentDepth };

	p_parser.skipNewLines();
	if (at(OGreater)) {
		return { .opening = t_opening, .arguments = {}, .closing = p_parser.advance() };
	}
	std::vector<TypeReference> v_arguments;
	while (true) {
		if (p_parser.atKeyword(KOpt)) {
			throw cde::GenericErrors::argument(std::format("Omission-only '{}' wrappers are not value type arguments", toString(KOpt)),
			                                   p_parser.peek().getSpan());
		}
		if (at(OUnderscore)) {
			v_arguments.emplace_back(p_parser.advance());
		} else {
			v_arguments.push_back(parse(/*inCast=*/false, /*allowArrays=*/true));
		}
		p_parser.skipNewLines();
		if (at(OGreater)) {
			break;
		}
		p_parser.expect(OComma);
		p_parser.skipNewLines();
	}
	return { .opening = t_opening, .arguments = std::move(v_arguments), .closing = p_parser.expect(OGreater) };
}

std::optional<int> TypeParser::angleListEnd (const int start) const {
	if (!at(OLess, start)) {
		return std::nullopt;
	}

	// Look ahead without advancing the cursor. Committed parsing checks grammar and nesting.
	int i_offset = start;
	int i_depth  = 0;
	while (true) {
		const Token        t_token  = p_parser.peek(i_offset++);
		const std::string &s_lexeme = t_token.getLexeme();
		if (s_lexeme == toString(OLess)) {
			++i_depth;
			continue;
		}
		if (s_lexeme == toString(OGreater)) {
			if (--i_depth == 0) {
				return i_offset;
			}
			continue;
		}
		const TokenType tt_type     = t_token.getType();
		const bool      b_typeToken = tt_type == TokenType::Identifier || tt_type == TokenType::Keyword || tt_type == TokenType::IntegerLiteral ||
		                         tt_type == TokenType::NewLine;
		const bool b_punctuation = s_lexeme == toString(OComma) || s_lexeme == toString(ODot) || s_lexeme == toString(OQuestion) ||
		                           s_lexeme == toString(OBracketOpen) || s_lexeme == toString(OBracketClose) || s_lexeme == toString(OUnderscore);
		if (!b_typeToken && !b_punctuation) {
			return std::nullopt;
		}
	}
}

std::optional<int> TypeParser::applicationEnd (const int start) const {
	const std::optional<int> opt_end = angleListEnd(start);
	return opt_end ? suffixEnd(*opt_end) : std::nullopt;
}

bool TypeParser::startsCallTypeArguments (const int nameOffset) const {
	const Token t_name = p_parser.peek(nameOffset);
	if (const Token t_opening = p_parser.peek(nameOffset + 1); t_name.getType() != TokenType::Identifier || !touches(t_name, t_opening)) {
		return false;
	}
	const std::optional<int> opt_end = angleListEnd(nameOffset + 1);
	if (!opt_end) {
		return false;
	}
	const Token t_arguments = p_parser.peek(*opt_end);
	return t_arguments.getLexeme() == toString(OParenOpen) && touches(p_parser.peek(*opt_end - 1), t_arguments);
}

std::optional<int> TypeParser::suffixEnd (const int start) const {
	int i_offset = start;
	while (true) {
		if (at(OQuestion, i_offset)) {
			++i_offset;
			continue;
		}
		if (!at(OBracketOpen, i_offset)) {
			return i_offset;
		}
		++i_offset;
		if (p_parser.peek(i_offset).getType() == TokenType::IntegerLiteral) {
			++i_offset;
		}
		if (!at(OBracketClose, i_offset)) {
			return std::nullopt;
		}
		++i_offset;
	}
}

bool TypeParser::startsNamedTypeReceiver () const {
	if (p_parser.peek().getType() != TokenType::Identifier) {
		return false;
	}
	const int          i_offset = nameEnd();
	std::optional<int> opt_end;
	if (at(OLess, i_offset)) {
		opt_end = applicationEnd(i_offset);
	} else if (at(OQuestion, i_offset) || (at(OBracketOpen, i_offset) && at(OBracketClose, i_offset + 1))) {
		opt_end = suffixEnd(i_offset);
	}
	return opt_end && at(OColonColon, *opt_end);
}

bool TypeParser::startsQualifiedTypeCheck () const {
	if (p_parser.peek().getType() != TokenType::Identifier) {
		return false;
	}
	const int i_offset = nameEnd();
	if (i_offset == 1 || startsNamedTypeReceiver() || startsCallTypeArguments(i_offset - 1)) {
		return false;
	}
	if (at(OLess, i_offset)) {
		const std::optional<int> opt_end = applicationEnd(i_offset);
		return opt_end && typeCanEndAt(*opt_end);
	}
	return at(OQuestion, i_offset);
}

std::optional<int> TypeParser::typedTargetEnd (const int start) const {
	const int   i_name = p_parser.atKeyword(KUnsigned, start) ? start + 1 : start;
	const Token t_name = p_parser.peek(i_name);
	if (!isTypeName(t_name)) {
		return std::nullopt;
	}
	const int                i_end      = t_name.getType() == TokenType::Identifier ? nameEnd(i_name) : i_name + 1;
	const std::optional<int> opt_target = at(OLess, i_end) ? applicationEnd(i_end) : suffixEnd(i_end);
	if (!opt_target || !Parser::isReceivingTarget(p_parser.peek(*opt_target))) {
		return std::nullopt;
	}
	return *opt_target + 1;
}

int TypeParser::nameEnd (const int start) const {
	int i_offset = start + 1;
	while (at(ODot, i_offset) && p_parser.peek(i_offset + 1).getType() == TokenType::Identifier) {
		i_offset += 2;
	}
	return i_offset;
}

bool TypeParser::typeCanEndAt (const int offset) const {
	const TokenType tt_type = p_parser.peek(offset).getType();
	return tt_type == TokenType::Operator || tt_type == TokenType::EndOfFile || tt_type == TokenType::NewLine || p_parser.atKeyword(KIs, offset);
}

TypeReference TypeParser::parse (bool inCast, bool allowArrays) {
	const TokenSpan ts_start   = p_parser.peek().getSpan();
	const bool      b_unsigned = p_parser.atKeyword(KUnsigned);
	if (b_unsigned) {
		p_parser.advance();
	}
	const Token t_name = parseName();

	std::optional<TypeArgumentList> opt_application;
	std::optional<Token>            opt_width;
	// In a cast, a following '<' may start a comparison.
	if (at(OLess)) {
		if (t_name.getType() == TokenType::Identifier) {
			if (const std::optional<int> opt_end = inCast ? applicationEnd(0) : std::nullopt; !inCast || (opt_end && typeCanEndAt(*opt_end))) {
				opt_application = arguments();
			}
		} else if (!inCast || (isWidthToken(p_parser.peek(1)) && at(OGreater, 2))) {
			opt_width = p_parser.p_delimitedParser.angles([this] () -> Token {
				return isWidthToken(p_parser.peek()) ? p_parser.advance() : p_parser.expect(TokenType::IntegerLiteral, "a numeric type width");
			});
		}
	}

	const std::optional<TokenSpan>        opt_nullable = nullableMarker();
	std::vector<TokenSpan>                v_arraySuffixes;
	std::vector<std::optional<Token>>     vopt_arraySizes;
	std::vector<std::optional<TokenSpan>> vopt_arrayNullability;
	while (allowArrays && at(OBracketOpen)) {
		const Token t_opening = p_parser.advance();
		vopt_arraySizes.push_back(p_parser.peek().getType() == TokenType::IntegerLiteral ? std::make_optional(p_parser.advance()) : std::nullopt);
		const Token t_closing = p_parser.expect(OBracketClose);
		v_arraySuffixes.push_back(TokenSpan::spanThrough(t_opening.getSpan(), t_closing.getSpan()));
		vopt_arrayNullability.push_back(nullableMarker());
	}
	const TokenSpan ts_span = TokenSpan::spanThrough(ts_start, p_parser.previous().getSpan());
	return TypeReference(t_name, b_unsigned, std::move(opt_width), ts_span, std::move(v_arraySuffixes), std::move(vopt_arraySizes), opt_nullable,
	                     std::move(vopt_arrayNullability), std::move(opt_application));
}

Token TypeParser::parseName () const {
	if (const Token t_token = p_parser.peek(); !isTypeName(t_token)) {
		throw cde::SyntaxErrors::expected("a type name", t_token);
	}
	const Token t_first = p_parser.advance();
	if (t_first.getType() != TokenType::Identifier || !at(ODot)) {
		return t_first;
	}
	std::string s_spelling = t_first.getLexeme();
	while (at(ODot)) {
		s_spelling += p_parser.advance().getLexeme();
		s_spelling += p_parser.expect(TokenType::Identifier, "a nested type name").getLexeme();
	}
	return { std::move(s_spelling), t_first.getType(), TokenSpan::spanThrough(t_first.getSpan(), p_parser.previous().getSpan()) };
}

std::optional<TokenSpan> TypeParser::nullableMarker () const {
	if (!at(OQuestion)) {
		return std::nullopt;
	}
	TokenSpan ts_marker = p_parser.advance().getSpan();
	if (at(OQuestion)) {
		throw cde::SyntaxErrors::expected(std::format("only one '{}' at this type position", toString(OQuestion)), p_parser.peek());
	}
	return ts_marker;
}

bool TypeParser::isWidthToken (const Token &token) const {
	return token.getType() == TokenType::IntegerLiteral || (p_parser.b_allowInferredOperatorWidth && token.getLexeme() == toString(KAuto));
}

bool TypeParser::startsTypeReceiver (const Token &token) {
	return token.getType() == TokenType::Keyword && (token.getLexeme() == toString(KUnsigned) || isPrimitiveType(token.getLexeme()));
}

bool TypeParser::at (const Operator op, const int offset) const {
	return p_parser.peek(offset).getLexeme() == toString(op);
}

}
