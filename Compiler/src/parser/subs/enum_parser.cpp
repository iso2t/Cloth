// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/type/type_reference.hpp>
#include <generic_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/enum_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

EnumDeclaration EnumParser::parse (const Token &keyword, std::optional<Token> visibility) {
	const Token t_name = p_parser.expect(TokenType::Identifier, "an enum name");
	if (at(Operator::oLess)) {
		throw cde::GenericErrors::syntax("Enum type parameters", p_parser.peek());
	}
	std::vector<EnumFieldDeclaration> v_fields;
	if (at(Operator::oParenOpen)) {
		v_fields = p_parser.p_delimitedParser.parentheses([this] () -> std::vector<EnumFieldDeclaration> {
			return p_parser.p_delimitedParser.commaSeparated(Operator::oParenClose, [this] () -> EnumFieldDeclaration { return parseField(); });
		});
	}

	std::vector<TypeDeclarationPtr>  v_nestedTypes;
	std::vector<EnumCaseDeclaration> v_cases;
	std::vector<MethodDeclaration>   v_operators;
	p_parser.skipNewLines();
	p_parser.p_delimitedParser.braces([this, &v_nestedTypes, &v_cases, &v_operators] () -> void {
		p_parser.skipNewLines();
		while (p_parser.startsTypeDeclaration()) {
			v_nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
		}
		v_cases = p_parser.p_delimitedParser.commaSeparated(
		        Operator::oBraceClose, [this] () -> bool { return at(Operator::oSemicolon) || p_parser.startsTypeDeclaration(); },
		        [this] () -> EnumCaseDeclaration { return parseCase(); });
		if (v_cases.empty()) {
			throw cde::SyntaxErrors::expected("at least one enum case", p_parser.peek());
		}
		if (!at(Operator::oBraceClose)) {
			p_parser.expect(Operator::oSemicolon);
			p_parser.skipNewLines();
			v_operators = parseMembers(v_nestedTypes);
		}
	});
	const TokenSpan ts_span = TokenSpan::span_through((visibility ? *visibility : keyword).get_span(), p_parser.previous().get_span());
	return { keyword, t_name, std::move(visibility), std::move(v_fields), std::move(v_cases), std::move(v_operators), ts_span, std::move(v_nestedTypes) };
}

EnumFieldDeclaration EnumParser::parseField () {
	std::optional<Token> opt_visibility = p_parser.atKeyword(Keyword::kPublic) ? std::make_optional(p_parser.advance()) : std::nullopt;
	if (!p_parser.atKeyword(Keyword::kDef)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' for an enum payload field", cloth::to_string(Keyword::kDef)), p_parser.peek());
	}
	p_parser.advance();
	TypeReference tr_type = p_parser.p_delimitedParser.angles([this] () -> TypeReference {
		p_parser.skipNewLines();
		TypeReference tr_parsedType = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
		p_parser.skipNewLines();
		return tr_parsedType;
	});
	const Token   t_name  = p_parser.expect(TokenType::Identifier, "an enum payload field name");
	ExpressionPtr ep_defaultValue;
	if (at(Operator::oAssign)) {
		p_parser.advance();
		ep_defaultValue = value();
	}
	return { t_name, std::move(tr_type), std::move(opt_visibility), std::move(ep_defaultValue) };
}

EnumCaseDeclaration EnumParser::parseCase () {
	const Token                t_name = p_parser.expect(TokenType::Identifier, "an enum case");
	std::vector<ExpressionPtr> v_values;
	if (at(Operator::oParenOpen)) {
		v_values = p_parser.p_delimitedParser.parentheses([this] () -> std::vector<ExpressionPtr> {
			return p_parser.p_delimitedParser.commaSeparated(Operator::oParenClose, [this] () -> ExpressionPtr { return value(); });
		});
	}
	return { t_name, std::move(v_values) };
}

std::vector<MethodDeclaration> EnumParser::parseMembers (std::vector<TypeDeclarationPtr> &nestedTypes) {
	std::vector<MethodDeclaration> v_operators;
	while (!at(Operator::oBraceClose)) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		if (!p_parser.atKeyword(Keyword::kOperator)) {
			throw cde::SyntaxErrors::expected(std::format("an '{}' declaration", cloth::to_string(Keyword::kOperator)), p_parser.peek());
		}
		v_operators.push_back(p_parser.p_functionParser.parseOperator());
		if (std::holds_alternative<ExpressionBody>(v_operators.back().body.value) && at(Operator::oSemicolon)) {
			p_parser.advance();
		} else if (!at(Operator::oBraceClose) && p_parser.peek().get_type() != TokenType::NewLine) {
			throw cde::SyntaxErrors::newline(p_parser.peek());
		}
		p_parser.skipNewLines();
	}
	return v_operators;
}

ExpressionPtr EnumParser::value () {
	return p_parser.p_functionParser.inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
}

bool EnumParser::at (Operator op) {
	return p_parser.peek().get_lexeme() == cloth::to_string(op);
}

}
