// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
#include <cde/generic_errors.hpp>
#include <cde/syntax_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

constexpr std::array PRIMITIVE_TYPES{
	kInteger, kFloat, kBoolean, kString, kCharacter, kVoid, kTypeinfo, kUsize, kIsize,
};

bool isPrimitiveType (std::string_view lexeme) {
	return std::ranges::any_of(PRIMITIVE_TYPES, [lexeme] (const Keyword keyword) -> bool { return lexeme == to_string(keyword); });
}

bool touches (const Token &token, const Token &next) {
	const TokenSpan ts_token = token.get_span();
	const TokenSpan ts_next  = next.get_span();
	return ts_token.endLine == ts_next.startLine && ts_token.endCol == ts_next.startCol;
}

}

TypeParser::ParentType TypeParser::parseParent (const std::string_view description) {
	const Token t_separator = p_parser.advance();
	p_parser.skipNewLines();
	if (const Token t_next = p_parser.peek(); t_next.get_type() != TokenType::Identifier) {
		throw cde::SyntaxErrors::expected(std::string(description), t_next);
	}
	TypeReference tr_type = parse();
	if (tr_type.hasNullability()) {
		throw cde::SyntaxErrors::expected("a non-nullable parent type", p_parser.previous());
	}
	return { .separator = t_separator, .type = std::move(tr_type) };
}

std::optional<TypeParameterList> TypeParser::parseTypeParameters () {
	if (!at(oLess)) {
		return std::nullopt;
	}
	const Token t_opening = p_parser.advance();
	p_parser.skipNewLines();
	std::vector<TypeParameterDeclaration> v_parameters;
	while (true) {
		const Token t_name = p_parser.expect(TokenType::Identifier, "a type parameter name");
		p_parser.skipNewLines();
		std::vector<TypeParameterBound> v_bounds;
		if (at(oColon)) {
			v_bounds = parseBounds();
		}
		std::optional<TypeReference> opt_defaultType;
		if (at(oAssign)) {
			p_parser.advance();
			p_parser.skipNewLines();
			opt_defaultType = parse(/*inCast=*/false, /*allowArrays=*/true);
		}
		v_parameters.push_back({ .name = t_name, .bounds = std::move(v_bounds), .defaultType = std::move(opt_defaultType) });
		p_parser.skipNewLines();
		if (at(oElvis)) {
			throw cde::GenericErrors::syntax(std::format("Type defaults written with '{}'", to_string(oElvis)), p_parser.peek());
		}
		if (at(oGreater)) {
			break;
		}
		p_parser.expect(oComma);
		p_parser.skipNewLines();
	}
	return TypeParameterList{ .opening = t_opening, .parameters = std::move(v_parameters), .closing = p_parser.expect(oGreater) };
}

std::vector<TypeParameterBound> TypeParser::parseBounds () {
	std::vector<TypeParameterBound> v_bounds;
	do {
		p_parser.advance();
		p_parser.skipNewLines();
		if (p_parser.atKeyword(kNew)) {
			const Token                       t_keyword    = p_parser.advance();
			std::vector<ParameterDeclaration> v_parameters = p_parser.p_functionParser.parseParameters();
			const TokenSpan                   ts_span      = TokenSpan::span_through(t_keyword.get_span(), p_parser.previous().get_span());
			v_bounds.push_back({ ConstructorBound{ ConstructorGuaranteeDeclaration(t_keyword, std::move(v_parameters), ts_span) } });
		} else {
			v_bounds.push_back({ parse(/*inCast=*/false, /*allowArrays=*/true) });
		}
		p_parser.skipNewLines();
	} while (at(oAmpersand));
	return v_bounds;
}

TypeArgumentList TypeParser::arguments () {
	const Token t_opening = p_parser.expect(oLess);
	if (i_argumentDepth >= MAX_TYPE_ARG_NESTING) {
		throw cde::GenericErrors::nesting(t_opening.get_span(), MAX_TYPE_ARG_NESTING);
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
	if (at(oGreater)) {
		return { t_opening, {}, p_parser.advance() };
	}
	std::vector<TypeReference> v_arguments;
	while (true) {
		if (p_parser.atKeyword(kOpt)) {
			throw cde::GenericErrors::argument(std::format("Omission-only '{}' wrappers are not value type arguments", to_string(kOpt)),
			                                   p_parser.peek().get_span());
		}
		if (at(oUnderscore)) {
			v_arguments.emplace_back(p_parser.advance());
		} else {
			v_arguments.push_back(parse(/*inCast=*/false, /*allowArrays=*/true));
		}
		p_parser.skipNewLines();
		if (at(oGreater)) {
			break;
		}
		p_parser.expect(oComma);
		p_parser.skipNewLines();
	}
	return { t_opening, std::move(v_arguments), p_parser.expect(oGreater) };
}

std::optional<int> TypeParser::angleListEnd (const int start) const {
	if (!at(oLess, start)) {
		return std::nullopt;
	}

	// Look ahead without advancing the cursor. Committed parsing checks grammar and nesting.
	int i_offset = start;
	int i_depth  = 0;
	while (true) {
		const Token        t_token  = p_parser.peek(i_offset++);
		const std::string &s_lexeme = t_token.get_lexeme();
		if (s_lexeme == to_string(oLess)) {
			++i_depth;
			continue;
		}
		if (s_lexeme == to_string(oGreater)) {
			if (--i_depth == 0) {
				return i_offset;
			}
			continue;
		}
		const TokenType tt_type     = t_token.get_type();
		const bool      b_typeToken = tt_type == TokenType::Identifier || tt_type == TokenType::Keyword || tt_type == TokenType::IntegerLiteral ||
		                         tt_type == TokenType::NewLine;
		const bool b_punctuation = s_lexeme == to_string(oComma) || s_lexeme == to_string(oDot) || s_lexeme == to_string(oQuestion) ||
		                           s_lexeme == to_string(oBracketOpen) || s_lexeme == to_string(oBracketClose) || s_lexeme == to_string(oUnderscore);
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
	const Token t_name    = p_parser.peek(nameOffset);
	const Token t_opening = p_parser.peek(nameOffset + 1);
	if (t_name.get_type() != TokenType::Identifier || !touches(t_name, t_opening)) {
		return false;
	}
	const std::optional<int> opt_end = angleListEnd(nameOffset + 1);
	if (!opt_end) {
		return false;
	}
	const Token t_arguments = p_parser.peek(*opt_end);
	return t_arguments.get_lexeme() == to_string(oParenOpen) && touches(p_parser.peek(*opt_end - 1), t_arguments);
}

std::optional<int> TypeParser::suffixEnd (const int start) const {
	int i_offset = start;
	while (true) {
		if (at(oQuestion, i_offset)) {
			++i_offset;
			continue;
		}
		if (!at(oBracketOpen, i_offset)) {
			return i_offset;
		}
		++i_offset;
		if (p_parser.peek(i_offset).get_type() == TokenType::IntegerLiteral) {
			++i_offset;
		}
		if (!at(oBracketClose, i_offset)) {
			return std::nullopt;
		}
		++i_offset;
	}
}

bool TypeParser::startsNamedTypeReceiver () const {
	if (p_parser.peek().get_type() != TokenType::Identifier) {
		return false;
	}
	const int          i_offset = nameEnd();
	std::optional<int> opt_end;
	if (at(oLess, i_offset)) {
		opt_end = applicationEnd(i_offset);
	} else if (at(oQuestion, i_offset) || (at(oBracketOpen, i_offset) && at(oBracketClose, i_offset + 1))) {
		opt_end = suffixEnd(i_offset);
	}
	return opt_end && at(oColonColon, *opt_end);
}

bool TypeParser::startsQualifiedTypeCheck () const {
	if (p_parser.peek().get_type() != TokenType::Identifier) {
		return false;
	}
	const int i_offset = nameEnd();
	if (i_offset == 1 || startsNamedTypeReceiver() || startsCallTypeArguments(i_offset - 1)) {
		return false;
	}
	if (at(oLess, i_offset)) {
		const std::optional<int> opt_end = applicationEnd(i_offset);
		return opt_end && typeCanEndAt(*opt_end);
	}
	return at(oQuestion, i_offset);
}

int TypeParser::nameEnd () const {
	int i_offset = 1;
	while (at(oDot, i_offset) && p_parser.peek(i_offset + 1).get_type() == TokenType::Identifier) {
		i_offset += 2;
	}
	return i_offset;
}

bool TypeParser::typeCanEndAt (const int offset) const {
	const TokenType tt_type = p_parser.peek(offset).get_type();
	return tt_type == TokenType::Operator || tt_type == TokenType::EndOfFile || tt_type == TokenType::NewLine || p_parser.atKeyword(kIs, offset);
}

TypeReference TypeParser::parse (bool inCast, bool allowArrays) {
	const TokenSpan ts_start   = p_parser.peek().get_span();
	const bool      b_unsigned = p_parser.atKeyword(kUnsigned);
	if (b_unsigned) {
		p_parser.advance();
	}
	const Token t_name = parseName();

	std::optional<TypeArgumentList> opt_application;
	std::optional<Token>            opt_width;
	// In a cast, a following '<' may start a comparison.
	if (at(oLess)) {
		if (t_name.get_type() == TokenType::Identifier) {
			const std::optional<int> opt_end = inCast ? applicationEnd(0) : std::nullopt;
			if (!inCast || (opt_end && typeCanEndAt(*opt_end))) {
				opt_application = arguments();
			}
		} else if (!inCast || (isWidthToken(p_parser.peek(1)) && at(oGreater, 2))) {
			opt_width = p_parser.p_delimitedParser.angles([this] () -> Token {
				return isWidthToken(p_parser.peek()) ? p_parser.advance() : p_parser.expect(TokenType::IntegerLiteral, "a numeric type width");
			});
		}
	}

	const std::optional<TokenSpan>        opt_nullable = nullableMarker();
	std::vector<TokenSpan>                v_arraySuffixes;
	std::vector<std::optional<Token>>     vopt_arraySizes;
	std::vector<std::optional<TokenSpan>> vopt_arrayNullability;
	while (allowArrays && at(oBracketOpen)) {
		const Token t_opening = p_parser.advance();
		vopt_arraySizes.push_back(p_parser.peek().get_type() == TokenType::IntegerLiteral ? std::make_optional(p_parser.advance()) : std::nullopt);
		const Token t_closing = p_parser.expect(oBracketClose);
		v_arraySuffixes.push_back(TokenSpan::span_through(t_opening.get_span(), t_closing.get_span()));
		vopt_arrayNullability.push_back(nullableMarker());
	}
	const TokenSpan ts_span = TokenSpan::span_through(ts_start, p_parser.previous().get_span());
	return TypeReference(t_name, b_unsigned, std::move(opt_width), ts_span, std::move(v_arraySuffixes), std::move(vopt_arraySizes), opt_nullable,
	                     std::move(vopt_arrayNullability), std::move(opt_application));
}

Token TypeParser::parseName () const {
	if (const Token t_token = p_parser.peek();
	    t_token.get_type() != TokenType::Identifier && !(t_token.get_type() == TokenType::Keyword && isPrimitiveType(t_token.get_lexeme()))) {
		throw cde::SyntaxErrors::expected("a type name", t_token);
	}
	const Token t_first = p_parser.advance();
	if (t_first.get_type() != TokenType::Identifier || !at(oDot)) {
		return t_first;
	}
	std::string s_spelling = t_first.get_lexeme();
	while (at(oDot)) {
		s_spelling += p_parser.advance().get_lexeme();
		s_spelling += p_parser.expect(TokenType::Identifier, "a nested type name").get_lexeme();
	}
	return { std::move(s_spelling), t_first.get_type(), TokenSpan::span_through(t_first.get_span(), p_parser.previous().get_span()) };
}

std::optional<TokenSpan> TypeParser::nullableMarker () const {
	if (!at(oQuestion)) {
		return std::nullopt;
	}
	TokenSpan ts_marker = p_parser.advance().get_span();
	if (at(oQuestion)) {
		throw cde::SyntaxErrors::expected(std::format("only one '{}' at this type position", to_string(oQuestion)), p_parser.peek());
	}
	return ts_marker;
}

bool TypeParser::isWidthToken (const Token &token) const {
	return token.get_type() == TokenType::IntegerLiteral || (p_parser.b_allowInferredOperatorWidth && token.get_lexeme() == to_string(kAuto));
}

bool TypeParser::startsTypeReceiver (const Token &token) {
	return token.get_type() == TokenType::Keyword && (token.get_lexeme() == to_string(kUnsigned) || isPrimitiveType(token.get_lexeme()));
}

bool TypeParser::at (const Operator op, const int offset) const {
	return p_parser.peek(offset).get_lexeme() == to_string(op);
}

}
