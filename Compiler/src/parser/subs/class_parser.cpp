// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/class_expression.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <cde/syntax_errors.hpp>
#include <parser/parser.hpp>
#include <parser/subs/class_parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {

TopLevelStateDeclaration ClassParser::parseTopLevelState (const Token &typedefKeyword, const Token &keyword, TypeReference payload) const {
	p_parser.skipNewLines();
	std::optional<Token> opt_visibility = p_parser.parseVisibility();
	const Token          t_name         = p_parser.expect(TokenType::Identifier, "a state name");
	if (!p_parser.atKeyword(kFor)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' before the state host contract", to_string(kFor)), p_parser.peek());
	}
	p_parser.advance();
	TypeReference tr_host = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
	p_parser.skipNewLines();
	std::vector<TypeDeclarationPtr> v_nestedTypes;
	std::vector<StateMemberPtr>     v_members = parseStateBody(&v_nestedTypes);
	return { keyword,
		 std::move(opt_visibility),
		 t_name,
		 std::move(payload),
		 std::move(tr_host),
		 std::move(v_members),
		 TokenSpan::span_through(typedefKeyword.get_span(), p_parser.previous().get_span()),
		 std::move(v_nestedTypes) };
}

ClassDeclaration ClassParser::parse (const Token &keyword, std::optional<Token> visibility, std::optional<Token> finalKeyword) const {
	const Token                            t_name             = p_parser.expect(TokenType::Identifier, "a class name");
	std::optional<TypeParameterList>       opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
	std::vector<ClassParameterDeclaration> v_parameters;
	if (at(oParenOpen)) {
		v_parameters = p_parser.p_delimitedParser.parentheses([this] () -> std::vector<ClassParameterDeclaration> {
			return p_parser.p_delimitedParser.commaSeparated(oParenClose,
			                                                 [this] () -> ClassParameterDeclaration { return parseHeaderParameter(); });
		});
	}
	std::vector<ClassParentClause> v_parents = p_parser.p_typeParser.parseParents(
	        "a class or interface parent name", [this] (TypeReference type, const Token &separator) -> ClassParentClause {
		        std::optional<std::vector<CallArgument>> opt_arguments;
		        if (at(oParenOpen)) {
			        opt_arguments = p_parser.p_functionParser.inBody(
			                [this] () -> std::vector<CallArgument> { return p_parser.p_expressionParser.parseArgumentList(); });
		        }
		        return { .type                 = std::move(type),
			         .constructorArguments = std::move(opt_arguments),
			         .span                 = TokenSpan::span_through(separator.get_span(), p_parser.previous().get_span()) };
	        });
	const int i_headerEnd = p_parser.i_currentIndex;
	p_parser.skipNewLines();
	std::vector<TypeDeclarationPtr> v_nestedTypes;
	std::vector<ClassMember>        v_members;
	if (at(oBraceOpen)) {
		v_members = p_parser.p_delimitedParser.braces(
		        [this, &t_name, &v_nestedTypes] () -> std::vector<ClassMember> { return parseMembers(t_name, v_nestedTypes); });
	} else {
		p_parser.i_currentIndex = i_headerEnd;
	}
	const TokenSpan ts_start = (visibility ? *visibility : finalKeyword ? *finalKeyword : keyword).get_span();
	const TokenSpan ts_span  = TokenSpan::span_through(ts_start, p_parser.previous().get_span());
	return { keyword,
		 t_name,
		 std::move(visibility),
		 std::move(finalKeyword),
		 std::move(v_parameters),
		 std::move(v_members),
		 ts_span,
		 std::move(v_parents),
		 std::move(opt_typeParameters),
		 std::move(v_nestedTypes) };
}

ClassParameterDeclaration ClassParser::parseHeaderParameter () const {
	std::optional<Token> opt_visibility = p_parser.parseVisibility();
	if (!p_parser.atKeyword(kDef)) {
		if (opt_visibility) {
			throw cde::SyntaxErrors::expected(std::format("'{}' after '{}' in a class header", to_string(kDef), to_string(kPublic)),
			                                  p_parser.peek());
		}
		return { .parameter = p_parser.p_functionParser.parseParameter() };
	}
	const Token          t_keyword = p_parser.advance();
	TypeReference        tr_type   = parseFieldType();
	const Token          t_name    = p_parser.expect(TokenType::Identifier, "a field parameter name");
	std::optional<Token> opt_delimiter;
	ExpressionPtr        ep_fallback;
	if (at(oElvis)) {
		opt_delimiter = p_parser.advance();
		ep_fallback   = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
	}
	ParameterDeclaration pd_parameter{
		.type             = std::move(tr_type),
		.name             = t_name,
		.span             = TokenSpan::span_through(t_keyword.get_span(), p_parser.previous().get_span()),
		.defaultDelimiter = std::move(opt_delimiter),
		.defaultValue     = std::move(ep_fallback),
	};
	return { .parameter = std::move(pd_parameter), .fieldKeyword = t_keyword, .visibility = std::move(opt_visibility) };
}

std::vector<ClassMember> ClassParser::parseMembers (const Token &owner, std::vector<TypeDeclarationPtr> &nestedTypes) const {
	std::vector<ClassMember> v_members;
	p_parser.skipNewLines();
	while (!at(oBraceClose)) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		v_members.push_back(parseMember(owner));
		const auto &[value]                   = v_members.back();
		const MethodDeclaration   *md_method  = std::get_if<MethodDeclaration>(&value);
		const StateClassMemberPtr *scmp_state = std::get_if<StateClassMemberPtr>(&value);
		const bool b_allowsTerminator         = std::holds_alternative<ClassFieldDeclaration>(value) || (md_method && md_method->body.get_equals()) ||
		                                (scmp_state && (dynamic_cast<const StateMethodContractDeclaration *>(scmp_state->get()) ||
		                                                dynamic_cast<const StateAttachmentDeclaration *>(scmp_state->get())));
		finishMember(b_allowsTerminator, kClass);
	}
	return v_members;
}

ClassMember ClassParser::parseMember (const Token &owner) const {
	std::optional<Token>              opt_visibility = p_parser.parseVisibility();
	std::optional<InitialStatePrefix> opt_initial;
	if (p_parser.atKeyword(kDefault)) {
		opt_initial = parseInitialState();
		if (!p_parser.atKeyword(kState)) {
			throw cde::SyntaxErrors::expected(std::format("a '{}' declaration or attachment after '{}'", to_string(kState), to_string(kDefault)),
			                                  p_parser.peek());
		}
	}
	std::optional<TokenSpan> opt_virtual, opt_override;
	if (p_parser.atKeyword(kVirtual)) {
		opt_virtual = p_parser.advance().get_span();
	} else if (p_parser.atKeyword(kOverride)) {
		opt_override = p_parser.advance().get_span();
	}
	if ((opt_override || opt_virtual) && !p_parser.atKeyword(kMt)) {
		throw cde::SyntaxErrors::expected(std::format("a '{}' declaration after the method modifier", to_string(kMt)), p_parser.peek());
	}
	if (p_parser.atKeyword(kFn)) {
		throw cde::SyntaxErrors::functionInType(p_parser.peek());
	}
	if (p_parser.atKeyword(kDef)) {
		return { parseField(std::move(opt_visibility)) };
	}
	if (p_parser.atKeyword(kMt)) {
		return { p_parser.p_functionParser.parseMethod(std::move(opt_visibility), opt_override, opt_virtual,
			                                       /*allowIntrinsic=*/true, /*allowTypeParameters=*/true) };
	}
	if (p_parser.atKeyword(kOperator)) {
		return { p_parser.p_functionParser.parseOperator(std::move(opt_visibility)) };
	}
	if (p_parser.atKeyword(kState)) {
		return { parseState(std::move(opt_visibility), std::move(opt_initial)) };
	}
	if (at(oTilde)) {
		return { parseDestructor(owner, opt_visibility) };
	}
	if (p_parser.peek().get_type() == TokenType::Identifier) {
		return { parseConstructor(owner, opt_visibility) };
	}
	throw cde::SyntaxErrors::expected("a class member", p_parser.peek());
}

InitialStatePrefix ClassParser::parseInitialState () const {
	const Token t_keyword = p_parser.advance();
	p_parser.skipNewLines();
	ExpressionPtr ep_payload;
	if (at(oParenOpen)) {
		ep_payload = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr {
			return p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr {
				p_parser.skipNewLines();
				ExpressionPtr ep_expression = p_parser.p_expressionParser.parseExpression();
				p_parser.skipNewLines();
				return ep_expression;
			});
		});
	}
	const Token t_end = ep_payload ? p_parser.previous() : t_keyword;
	p_parser.skipNewLines();
	return { .keyword = t_keyword, .payload = std::move(ep_payload), .span = TokenSpan::span_through(t_keyword.get_span(), t_end.get_span()) };
}

StateClassMemberPtr ClassParser::parseState (std::optional<Token> visibility, std::optional<InitialStatePrefix> initial) const {
	const Token     t_keyword   = p_parser.advance();
	const Token     t_typeStart = p_parser.peek();
	const TokenSpan ts_start    = visibility ? visibility->get_span() : (initial ? initial->span : t_keyword.get_span());
	if (t_typeStart.get_type() == TokenType::Identifier) {
		const Token t_name = p_parser.advance();
		return std::make_shared<StateAttachmentDeclaration>(t_keyword, t_name, std::move(initial), TokenSpan::span_through(ts_start, t_name.get_span()),
		                                                    std::move(visibility));
	}
	std::vector<TypeReference> v_types = p_parser.parseTypeList();
	if (at(oColonColon)) {
		if (initial) {
			throw cde::SyntaxErrors::expected("a state name", p_parser.peek());
		}
		p_parser.advance();
		const Token                       t_name             = p_parser.expect(TokenType::Identifier, "a state-method name");
		std::optional<TypeParameterList>  opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
		std::vector<ParameterDeclaration> v_parameters       = p_parser.p_functionParser.parseParameters();
		return std::make_shared<StateMethodContractDeclaration>(t_keyword, std::move(visibility), t_name, std::move(v_types), std::move(v_parameters),
		                                                        TokenSpan::span_through(ts_start, p_parser.previous().get_span()),
		                                                        std::move(opt_typeParameters));
	}
	if (v_types.size() != 1) {
		const Token &t_unexpected = v_types.size() > 1 ? v_types[1].get_name() : t_typeStart;
		throw cde::SyntaxErrors::expected(std::format("one payload type in a nested '{}' declaration", to_string(kState)), t_unexpected);
	}
	const Token t_name = p_parser.expect(TokenType::Identifier, "a state name");
	p_parser.skipNewLines();
	std::vector<StateMemberPtr> v_members = parseStateBody();
	return std::make_shared<NestedStateDeclaration>(t_keyword, std::move(visibility), t_name, std::move(v_types.front()), std::move(initial),
	                                                std::move(v_members), TokenSpan::span_through(ts_start, p_parser.previous().get_span()));
}

std::vector<StateMemberPtr> ClassParser::parseStateBody (std::vector<TypeDeclarationPtr> *nestedTypes) const {
	return p_parser.p_delimitedParser.braces([this, nestedTypes] () -> std::vector<StateMemberPtr> {
		std::vector<StateMemberPtr> v_members;
		const bool                  b_previousStateBody = std::exchange(p_parser.b_inStateBody, true);
		try {
			p_parser.skipNewLines();
			while (!at(oBraceClose)) {
				if (nestedTypes && p_parser.startsTypeDeclaration()) {
					nestedTypes->emplace_back(p_parser.parseNestedTypeDeclaration());
					continue;
				}
				std::optional<Token> opt_visibility = p_parser.parseVisibility();
				if (p_parser.atKeyword(kFn)) {
					throw cde::SyntaxErrors::functionInType(p_parser.peek());
				}
				bool b_allowsTerminator = false;
				if (p_parser.atKeyword(kMt)) {
					MethodDeclaration md_method = p_parser.p_functionParser.parseMethod(
					        std::move(opt_visibility), std::nullopt, std::nullopt, /*allowIntrinsic=*/false, /*allowTypeParameters=*/true);
					b_allowsTerminator = md_method.body.get_equals() != nullptr;
					v_members.push_back(std::make_shared<MethodDeclaration>(std::move(md_method)));
				} else if (p_parser.atKeyword(kInit)) {
					const Token t_keyword = p_parser.advance();
					v_members.push_back(std::make_shared<StateInitializerDeclaration>(t_keyword, p_parser.p_functionParser.parseBlockBody(),
					                                                                  std::move(opt_visibility)));
				} else {
					throw cde::SyntaxErrors::expected(std::format("an '{}' block or a '{}' declaration", to_string(kInit), to_string(kMt)),
					                                  p_parser.peek());
				}
				finishMember(b_allowsTerminator, kState);
			}
		} catch (...) {
			p_parser.b_inStateBody = b_previousStateBody;
			throw;
		}
		p_parser.b_inStateBody = b_previousStateBody;
		return v_members;
	});
}

void ClassParser::finishMember (const bool allowsTerminator, const Keyword kind) const {
	if (const Token t_next = p_parser.peek(); allowsTerminator && p_parser.isSemiColon(t_next)) {
		p_parser.advance();
	} else if (t_next.get_lexeme() != to_string(oBraceClose) && t_next.get_type() != TokenType::NewLine) {
		if (allowsTerminator) {
			throw cde::SyntaxErrors::newline(t_next);
		}
		throw cde::SyntaxErrors::expected(std::format("a newline or '{}' after a brace-bodied {} member", to_string(oBraceClose), to_string(kind)),
		                                  t_next);
	}
	p_parser.skipNewLines();
}

ClassFieldDeclaration ClassParser::parseField (std::optional<Token> visibility) const {
	const Token   t_keyword = p_parser.advance();
	TypeReference tr_type   = parseFieldType();
	const Token   t_name    = p_parser.expect(TokenType::Identifier, "a class field name");
	p_parser.expect(oAssign);
	ExpressionPtr   ep_initializer = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
	const TokenSpan ts_span        = TokenSpan::span_through((visibility ? *visibility : t_keyword).get_span(), p_parser.previous().get_span());
	return { .keyword     = t_keyword,
		 .type        = std::move(tr_type),
		 .name        = t_name,
		 .initializer = std::move(ep_initializer),
		 .visibility  = std::move(visibility),
		 .span        = ts_span };
}

TypeReference ClassParser::parseFieldType () const {
	return p_parser.p_delimitedParser.angles([this] () -> TypeReference {
		p_parser.skipNewLines();
		TypeReference tr_type = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
		p_parser.skipNewLines();
		return tr_type;
	});
}

ConstructorDeclaration ClassParser::parseConstructor (const Token &owner, const std::optional<Token> &visibility) const {
	const Token                       t_name       = parseMemberName(owner, "constructor");
	std::vector<ParameterDeclaration> v_parameters = p_parser.p_functionParser.parseParameters();
	const Token                       t_colon      = p_parser.expect(oColon);
	std::vector<CallArgument>         v_arguments =
	        p_parser.p_functionParser.inBody([this] () -> std::vector<CallArgument> { return p_parser.p_expressionParser.parseArgumentList(); });
	ConstructorDelegation cd_delegation(t_name, std::move(v_arguments), TokenSpan::span_through(t_colon.get_span(), p_parser.previous().get_span()));
	return { t_name, std::move(v_parameters), std::move(cd_delegation), p_parser.p_functionParser.parseBlockBody(), visibility };
}

DestructorDeclaration ClassParser::parseDestructor (const Token &owner, const std::optional<Token> &visibility) const {
	const Token t_operator = p_parser.advance();
	const Token t_name     = parseMemberName(owner, "destructor");
	return { t_operator, t_name, p_parser.p_functionParser.parseBlockBody(), visibility };
}

Token ClassParser::parseMemberName (const Token &owner, std::string_view kind) const {
	if (const Token t_name = p_parser.peek(); t_name.get_type() != TokenType::Identifier || t_name.get_name_key() != owner.get_name_key()) {
		throw cde::SyntaxErrors::expected(std::format("'{}' as the {} name", owner.get_lexeme(), kind), t_name);
	}
	return p_parser.advance();
}

bool ClassParser::at (const Operator op) const {
	return p_parser.peek().get_lexeme() == to_string(op);
}

}
