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
#include <generic_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

constexpr std::array<Keyword, 9> PRIMITIVE_TYPES{
	Keyword::kInteger, Keyword::kFloat,    Keyword::kBoolean, Keyword::kString, Keyword::kCharacter,
	Keyword::kVoid,    Keyword::kTypeinfo, Keyword::kUsize,   Keyword::kIsize,
};

bool isPrimitiveType (std::string_view lexeme) {
	return std::any_of(PRIMITIVE_TYPES.begin(), PRIMITIVE_TYPES.end(), [lexeme] (Keyword keyword) -> bool { return lexeme == cloth::to_string(keyword); });
}

bool touches (const Token &token, const Token &next) {
	const TokenSpan ts_token = token.get_span();
	const TokenSpan ts_next  = next.get_span();
	return ts_token.endLine == ts_next.startLine && ts_token.endCol == ts_next.startCol;
}

}

TypeParser::ParentType TypeParser::parseParent (std::string_view description) {
	const Token t_separator = p_parser.advance();
	p_parser.skipNewLines();
	const Token t_next = p_parser.peek();
	if (t_next.get_type() != TokenType::Identifier) {
		throw cde::SyntaxErrors::expected(std::string(description), t_next);
	}
	TypeReference tr_type = parse();
	if (tr_type.hasNullability()) {
		throw cde::SyntaxErrors::expected("a non-nullable parent type", p_parser.previous());
	}
	return { t_separator, std::move(tr_type) };
}

std::optional<TypeParameterList> TypeParser::parseTypeParameters () {
	if (!at(Operator::oLess)) {
		return std::nullopt;
	}
	const Token t_opening = p_parser.advance();
	p_parser.skipNewLines();
	std::vector<TypeParameterDeclaration> v_parameters;
	while (true) {
		const Token t_name = p_parser.expect(TokenType::Identifier, "a type parameter name");
		p_parser.skipNewLines();
		std::vector<TypeParameterBound> v_bounds;
		if (at(Operator::oColon)) {
			v_bounds = parseBounds();
		}
		std::optional<TypeReference> opt_defaultType;
		if (at(Operator::oAssign)) {
			p_parser.advance();
			p_parser.skipNewLines();
			opt_defaultType = parse(/*inCast=*/false, /*allowArrays=*/true);
		}
		v_parameters.push_back({ t_name, std::move(v_bounds), std::move(opt_defaultType) });
		p_parser.skipNewLines();
		if (at(Operator::oElvis)) {
			throw cde::GenericErrors::syntax(std::format("Type defaults written with '{}'", cloth::to_string(Operator::oElvis)), p_parser.peek());
		}
		if (at(Operator::oGreater)) {
			break;
		}
		p_parser.expect(Operator::oComma);
		p_parser.skipNewLines();
	}
	return TypeParameterList{ t_opening, std::move(v_parameters), p_parser.expect(Operator::oGreater) };
}

std::vector<TypeParameterBound> TypeParser::parseBounds () {
	std::vector<TypeParameterBound> v_bounds;
	do {
		p_parser.advance();
		p_parser.skipNewLines();
		if (p_parser.atKeyword(Keyword::kNew)) {
			const Token                       t_keyword    = p_parser.advance();
			std::vector<ParameterDeclaration> v_parameters = p_parser.p_functionParser.parseParameters();
			const TokenSpan                   ts_span      = TokenSpan::span_through(t_keyword.get_span(), p_parser.previous().get_span());
			v_bounds.push_back({ ConstructorBound{ ConstructorGuaranteeDeclaration(t_keyword, std::move(v_parameters), ts_span) } });
		} else {
			v_bounds.push_back({ parse(/*inCast=*/false, /*allowArrays=*/true) });
		}
		p_parser.skipNewLines();
	} while (at(Operator::oAmpersand));
	return v_bounds;
}

TypeArgumentList TypeParser::arguments () {
	const Token t_opening = p_parser.expect(Operator::oLess);
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
	if (at(Operator::oGreater)) {
		return { t_opening, {}, p_parser.advance() };
	}
	std::vector<TypeReference> v_arguments;
	while (true) {
		if (p_parser.atKeyword(Keyword::kOpt)) {
			throw cde::GenericErrors::argument(std::format("Omission-only '{}' wrappers are not value type arguments",
			                                               cloth::to_string(Keyword::kOpt)),
			                                   p_parser.peek().get_span());
		}
		if (at(Operator::oUnderscore)) {
			v_arguments.emplace_back(p_parser.advance());
		} else {
			v_arguments.push_back(parse(/*inCast=*/false, /*allowArrays=*/true));
		}
		p_parser.skipNewLines();
		if (at(Operator::oGreater)) {
			break;
		}
		p_parser.expect(Operator::oComma);
		p_parser.skipNewLines();
	}
	return { t_opening, std::move(v_arguments), p_parser.expect(Operator::oGreater) };
}

std::optional<int> TypeParser::angleListEnd (int start) {
	if (!at(Operator::oLess, start)) {
		return std::nullopt;
	}

	// Look ahead without advancing the cursor. Committed parsing checks grammar and nesting.
	int i_offset = start;
	int i_depth  = 0;
	while (true) {
		const Token        t_token  = p_parser.peek(i_offset++);
		const std::string &s_lexeme = t_token.get_lexeme();
		if (s_lexeme == cloth::to_string(Operator::oLess)) {
			++i_depth;
			continue;
		}
		if (s_lexeme == cloth::to_string(Operator::oGreater)) {
			if (--i_depth == 0) {
				return i_offset;
			}
			continue;
		}
		const TokenType tt_type     = t_token.get_type();
		const bool      b_typeToken = tt_type == TokenType::Identifier || tt_type == TokenType::Keyword || tt_type == TokenType::IntegerLiteral ||
		                         tt_type == TokenType::NewLine;
		const bool b_punctuation = s_lexeme == cloth::to_string(Operator::oComma) || s_lexeme == cloth::to_string(Operator::oDot) ||
		                           s_lexeme == cloth::to_string(Operator::oQuestion) || s_lexeme == cloth::to_string(Operator::oBracketOpen) ||
		                           s_lexeme == cloth::to_string(Operator::oBracketClose) || s_lexeme == cloth::to_string(Operator::oUnderscore);
		if (!b_typeToken && !b_punctuation) {
			return std::nullopt;
		}
	}
}

std::optional<int> TypeParser::applicationEnd (int start) {
	const std::optional<int> opt_end = angleListEnd(start);
	return opt_end ? suffixEnd(*opt_end) : std::nullopt;
}

bool TypeParser::startsCallTypeArguments (int nameOffset) {
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
	return t_arguments.get_lexeme() == cloth::to_string(Operator::oParenOpen) && touches(p_parser.peek(*opt_end - 1), t_arguments);
}

std::optional<int> TypeParser::suffixEnd (int start) {
	int i_offset = start;
	while (true) {
		if (at(Operator::oQuestion, i_offset)) {
			++i_offset;
			continue;
		}
		if (!at(Operator::oBracketOpen, i_offset)) {
			return i_offset;
		}
		++i_offset;
		if (p_parser.peek(i_offset).get_type() == TokenType::IntegerLiteral) {
			++i_offset;
		}
		if (!at(Operator::oBracketClose, i_offset)) {
			return std::nullopt;
		}
		++i_offset;
	}
}

bool TypeParser::startsNamedTypeReceiver () {
	if (p_parser.peek().get_type() != TokenType::Identifier) {
		return false;
	}
	const int          i_offset = nameEnd();
	std::optional<int> opt_end;
	if (at(Operator::oLess, i_offset)) {
		opt_end = applicationEnd(i_offset);
	} else if (at(Operator::oQuestion, i_offset) || (at(Operator::oBracketOpen, i_offset) && at(Operator::oBracketClose, i_offset + 1))) {
		opt_end = suffixEnd(i_offset);
	}
	return opt_end && at(Operator::oColonColon, *opt_end);
}

bool TypeParser::startsQualifiedTypeCheck () {
	if (p_parser.peek().get_type() != TokenType::Identifier) {
		return false;
	}
	const int i_offset = nameEnd();
	if (i_offset == 1 || startsNamedTypeReceiver() || startsCallTypeArguments(i_offset - 1)) {
		return false;
	}
	if (at(Operator::oLess, i_offset)) {
		const std::optional<int> opt_end = applicationEnd(i_offset);
		return opt_end && typeCanEndAt(*opt_end);
	}
	return at(Operator::oQuestion, i_offset);
}

int TypeParser::nameEnd () {
	int i_offset = 1;
	while (at(Operator::oDot, i_offset) && p_parser.peek(i_offset + 1).get_type() == TokenType::Identifier) {
		i_offset += 2;
	}
	return i_offset;
}

bool TypeParser::typeCanEndAt (int offset) {
	const TokenType tt_type = p_parser.peek(offset).get_type();
	return tt_type == TokenType::Operator || tt_type == TokenType::EndOfFile || tt_type == TokenType::NewLine || p_parser.atKeyword(Keyword::kIs, offset);
}

TypeReference TypeParser::parse (bool inCast, bool allowArrays) {
	const TokenSpan ts_start   = p_parser.peek().get_span();
	const bool      b_unsigned = p_parser.atKeyword(Keyword::kUnsigned);
	if (b_unsigned) {
		p_parser.advance();
	}
	const Token t_name = parseName();

	std::optional<TypeArgumentList> opt_application;
	std::optional<Token>            opt_width;
	// In a cast, a following '<' may start a comparison.
	if (at(Operator::oLess)) {
		if (t_name.get_type() == TokenType::Identifier) {
			const std::optional<int> opt_end = inCast ? applicationEnd(0) : std::nullopt;
			if (!inCast || (opt_end && typeCanEndAt(*opt_end))) {
				opt_application = arguments();
			}
		} else if (!inCast || (isWidthToken(p_parser.peek(1)) && at(Operator::oGreater, 2))) {
			opt_width = p_parser.p_delimitedParser.angles([this] () -> Token {
				return isWidthToken(p_parser.peek()) ? p_parser.advance() : p_parser.expect(TokenType::IntegerLiteral, "a numeric type width");
			});
		}
	}

	const std::optional<TokenSpan>        opt_nullable = nullableMarker();
	std::vector<TokenSpan>                v_arraySuffixes;
	std::vector<std::optional<Token>>     vopt_arraySizes;
	std::vector<std::optional<TokenSpan>> vopt_arrayNullability;
	while (allowArrays && at(Operator::oBracketOpen)) {
		const Token t_opening = p_parser.advance();
		vopt_arraySizes.push_back(p_parser.peek().get_type() == TokenType::IntegerLiteral ? std::make_optional(p_parser.advance()) : std::nullopt);
		const Token t_closing = p_parser.expect(Operator::oBracketClose);
		v_arraySuffixes.push_back(TokenSpan::span_through(t_opening.get_span(), t_closing.get_span()));
		vopt_arrayNullability.push_back(nullableMarker());
	}
	const TokenSpan ts_span = TokenSpan::span_through(ts_start, p_parser.previous().get_span());
	return TypeReference(t_name, b_unsigned, std::move(opt_width), ts_span, std::move(v_arraySuffixes), std::move(vopt_arraySizes), opt_nullable,
	                     std::move(vopt_arrayNullability), std::move(opt_application));
}

Token TypeParser::parseName () {
	const Token t_token = p_parser.peek();
	if (t_token.get_type() != TokenType::Identifier && !(t_token.get_type() == TokenType::Keyword && isPrimitiveType(t_token.get_lexeme()))) {
		throw cde::SyntaxErrors::expected("a type name", t_token);
	}
	const Token t_first = p_parser.advance();
	if (t_first.get_type() != TokenType::Identifier || !at(Operator::oDot)) {
		return t_first;
	}
	std::string s_spelling = t_first.get_lexeme();
	while (at(Operator::oDot)) {
		s_spelling += p_parser.advance().get_lexeme();
		s_spelling += p_parser.expect(TokenType::Identifier, "a nested type name").get_lexeme();
	}
	return { std::move(s_spelling), t_first.get_type(), TokenSpan::span_through(t_first.get_span(), p_parser.previous().get_span()) };
}

std::optional<TokenSpan> TypeParser::nullableMarker () {
	if (!at(Operator::oQuestion)) {
		return std::nullopt;
	}
	const TokenSpan ts_marker = p_parser.advance().get_span();
	if (at(Operator::oQuestion)) {
		throw cde::SyntaxErrors::expected(std::format("only one '{}' at this type position", cloth::to_string(Operator::oQuestion)), p_parser.peek());
	}
	return ts_marker;
}

bool TypeParser::isWidthToken (const Token &token) const {
	return token.get_type() == TokenType::IntegerLiteral ||
	       (p_parser.b_allowInferredOperatorWidth && token.get_lexeme() == cloth::to_string(Keyword::kAuto));
}

bool TypeParser::startsTypeReceiver (const Token &token) {
	return token.get_type() == TokenType::Keyword && (token.get_lexeme() == cloth::to_string(Keyword::kUnsigned) || isPrimitiveType(token.get_lexeme()));
}

bool TypeParser::at (Operator op, int offset) {
	return p_parser.peek(offset).get_lexeme() == cloth::to_string(op);
}

}
