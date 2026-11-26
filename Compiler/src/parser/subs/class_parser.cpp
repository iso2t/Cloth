// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/types/Declarations.rst
// See Documentation/language/objects/States.rst

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
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/class_parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {

TopLevelStateDeclaration ClassParser::parseTopLevelState (const Token &typedefKeyword, const Token &keyword, TypeReference payload) const {
	std::optional<Token> opt_visibility = p_parser.parseVisibility();
	const Token          t_name         = p_parser.expect(TokenType::Identifier, "a state name");
	if (!p_parser.atKeyword(KFor)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' before the state host contract", toString(KFor)), p_parser.peek());
	}
	p_parser.advance();
	TypeReference tr_host = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
	std::vector<TypeDeclarationPtr> v_nestedTypes;
	std::vector<StateMemberPtr>     v_members = parseStateBody(&v_nestedTypes);
	return { keyword,
		 std::move(opt_visibility),
		 t_name,
		 std::move(payload),
		 std::move(tr_host),
		 std::move(v_members),
		 TokenSpan::spanThrough(typedefKeyword.getSpan(), p_parser.previous().getSpan()),
		 std::move(v_nestedTypes) };
}

ClassDeclaration ClassParser::parse (const Token &keyword, std::optional<Token> visibility, std::optional<Token> finalKeyword) const {
	const Token                            t_name             = p_parser.expect(TokenType::Identifier, "a class name");
	std::optional<TypeParameterList>       opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
	std::vector<ClassParameterDeclaration> v_parameters;
	if (at(OParenOpen)) {
		v_parameters = p_parser.p_delimitedParser.parentheses([this] () -> std::vector<ClassParameterDeclaration> {
			return p_parser.p_delimitedParser.commaSeparated(OParenClose,
			                                                 [this] () -> ClassParameterDeclaration { return parseHeaderParameter(); });
		});
	}
	std::vector<ClassParentClause> v_parents = p_parser.p_typeParser.parseParents(
	        "a class or interface parent name", [this] (TypeReference type, const Token &separator) -> ClassParentClause {
		        std::optional<std::vector<CallArgument>> opt_arguments;
		        if (at(OParenOpen)) {
			        opt_arguments = p_parser.p_functionParser.inBody(
			                [this] () -> std::vector<CallArgument> { return p_parser.p_expressionParser.parseArgumentList(); });
		        }
		        return { .type                 = std::move(type),
			         .constructorArguments = std::move(opt_arguments),
			         .span                 = TokenSpan::spanThrough(separator.getSpan(), p_parser.previous().getSpan()) };
	        });
	std::vector<TypeDeclarationPtr> v_nestedTypes;
	std::vector<ClassMember>        v_members;
	if (!p_parser.consumeEmptyTypeHeader("class header")) {
		v_members = p_parser.p_delimitedParser.braces(
		        [this, &t_name, &v_nestedTypes] () -> std::vector<ClassMember> { return parseMembers(t_name, v_nestedTypes); });
	}
	const TokenSpan ts_start = (visibility ? *visibility : finalKeyword ? *finalKeyword : keyword).getSpan();
	const TokenSpan ts_span  = TokenSpan::spanThrough(ts_start, p_parser.previous().getSpan());
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
	if (!p_parser.atKeyword(KDef)) {
		if (opt_visibility) {
			throw cde::SyntaxErrors::expected(std::format("'{}' after '{}' in a class header", toString(KDef), toString(KPublic)), p_parser.peek());
		}
		return { .parameter = p_parser.p_functionParser.parseParameter() };
	}
	const Token          t_keyword = p_parser.advance();
	TypeReference        tr_type   = parseFieldType();
	const Token          t_name    = p_parser.expect(TokenType::Identifier, "a field parameter name");
	std::optional<Token> opt_delimiter;
	ExpressionPtr        ep_fallback;
	if (at(OElvis)) {
		opt_delimiter = p_parser.advance();
		ep_fallback   = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
	}
	ParameterDeclaration pd_parameter{
		.type             = std::move(tr_type),
		.name             = t_name,
		.span             = TokenSpan::spanThrough(t_keyword.getSpan(), p_parser.previous().getSpan()),
		.defaultDelimiter = std::move(opt_delimiter),
		.defaultValue     = std::move(ep_fallback),
	};
	return { .parameter = std::move(pd_parameter), .fieldKeyword = t_keyword, .visibility = std::move(opt_visibility) };
}

std::vector<ClassMember> ClassParser::parseMembers (const Token &owner, std::vector<TypeDeclarationPtr> &nestedTypes) const {
	std::vector<ClassMember> v_members;
	p_parser.parseDeclarationItems(/*braced=*/true, [&] (const PublicBlockOrigin &publicBlock) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration(publicBlock));
			return;
		}
		const auto &value = v_members.emplace_back(parseMember(owner), publicBlock).value;
		const auto *state = std::get_if<StateClassMemberPtr>(&value);
		if (std::holds_alternative<ClassFieldDeclaration>(value) || (state && (dynamic_cast<const StateMethodContractDeclaration *>(state->get()) ||
		                                                                       dynamic_cast<const StateAttachmentDeclaration *>(state->get())))) {
			p_parser.requireSemicolon("class member");
		}
	});
	return v_members;
}

ClassMember::Value ClassParser::parseMember (const Token &owner) const {
	std::optional<Token>              opt_visibility = p_parser.parseVisibility();
	std::optional<InitialStatePrefix> opt_initial;
	if (p_parser.atKeyword(KDefault)) {
		opt_initial = parseInitialState();
		if (!p_parser.atKeyword(KState)) {
			throw cde::SyntaxErrors::expected(std::format("a '{}' declaration or attachment after '{}'", toString(KState), toString(KDefault)),
			                                  p_parser.peek());
		}
	}
	std::optional<TokenSpan> opt_virtual, opt_override;
	if (p_parser.atKeyword(KVirtual)) {
		opt_virtual = p_parser.advance().getSpan();
	} else if (p_parser.atKeyword(KOverride)) {
		opt_override = p_parser.advance().getSpan();
	}
	if ((opt_override || opt_virtual) && !p_parser.atKeyword(KMt)) {
		throw cde::SyntaxErrors::expected(std::format("a '{}' declaration after the method modifier", toString(KMt)), p_parser.peek());
	}
	if (p_parser.atKeyword(KFn)) {
		throw cde::SyntaxErrors::functionInType(p_parser.peek());
	}
	if (p_parser.atKeyword(KDef)) {
		return parseField(std::move(opt_visibility));
	}
	if (p_parser.atKeyword(KMt)) {
		return p_parser.p_functionParser.parseMethod(std::move(opt_visibility), opt_override, opt_virtual,
		                                             /*allowIntrinsic=*/true, /*allowTypeParameters=*/true);
	}
	if (p_parser.atKeyword(KOperator)) {
		return p_parser.p_functionParser.parseOperator(std::move(opt_visibility));
	}
	if (p_parser.atKeyword(KState)) {
		return parseState(std::move(opt_visibility), std::move(opt_initial));
	}
	if (at(OTilde)) {
		return parseDestructor(owner, opt_visibility);
	}
	if (p_parser.peek().getType() == TokenType::Identifier) {
		return parseConstructor(owner, opt_visibility);
	}
	throw cde::SyntaxErrors::expected("a class member", p_parser.peek());
}

InitialStatePrefix ClassParser::parseInitialState () const {
	const Token t_keyword = p_parser.advance();
	ExpressionPtr ep_payload;
	if (at(OParenOpen)) {
		ep_payload = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr {
			return p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
		});
	}
	const Token t_end = ep_payload ? p_parser.previous() : t_keyword;
	return { .keyword = t_keyword, .payload = std::move(ep_payload), .span = TokenSpan::spanThrough(t_keyword.getSpan(), t_end.getSpan()) };
}

StateClassMemberPtr ClassParser::parseState (std::optional<Token> visibility, std::optional<InitialStatePrefix> initial) const {
	const Token     t_keyword   = p_parser.advance();
	const Token     t_typeStart = p_parser.peek();
	const TokenSpan ts_start    = visibility ? visibility->getSpan() : initial ? initial->span : t_keyword.getSpan();
	if (t_typeStart.getType() == TokenType::Identifier) {
		const Token t_name = p_parser.advance();
		return std::make_shared<StateAttachmentDeclaration>(t_keyword, t_name, std::move(initial), TokenSpan::spanThrough(ts_start, t_name.getSpan()),
		                                                    std::move(visibility));
	}
	std::vector<TypeReference> v_types = p_parser.parseTypeList();
	if (at(OColonColon)) {
		if (initial) {
			throw cde::SyntaxErrors::expected("a state name", p_parser.peek());
		}
		p_parser.advance();
		const Token                       t_name             = p_parser.expect(TokenType::Identifier, "a state-method name");
		std::optional<TypeParameterList>  opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
		std::vector<ParameterDeclaration> v_parameters       = p_parser.p_functionParser.parseParameters();
		return std::make_shared<StateMethodContractDeclaration>(t_keyword, std::move(visibility), t_name, std::move(v_types), std::move(v_parameters),
		                                                        TokenSpan::spanThrough(ts_start, p_parser.previous().getSpan()),
		                                                        std::move(opt_typeParameters));
	}
	if (v_types.size() != 1) {
		const Token &t_unexpected = v_types.size() > 1 ? v_types[1].getName() : t_typeStart;
		throw cde::SyntaxErrors::expected(std::format("one payload type in a nested '{}' declaration", toString(KState)), t_unexpected);
	}
	const Token t_name = p_parser.expect(TokenType::Identifier, "a state name");
	std::vector<StateMemberPtr> v_members = parseStateBody();
	return std::make_shared<NestedStateDeclaration>(t_keyword, std::move(visibility), t_name, std::move(v_types.front()), std::move(initial),
	                                                std::move(v_members), TokenSpan::spanThrough(ts_start, p_parser.previous().getSpan()));
}

std::vector<StateMemberPtr> ClassParser::parseStateBody (std::vector<TypeDeclarationPtr> *nestedTypes) const {
	if (p_parser.consumeEmptyTypeHeader("state header")) {
		return {};
	}
	return p_parser.p_delimitedParser.braces([this, nestedTypes] () -> std::vector<StateMemberPtr> {
		std::vector<StateMemberPtr> v_members;
		const bool                  b_previousStateBody = std::exchange(p_parser.b_inStateBody, true);
		try {
			while (!at(OBraceClose)) {
				if (nestedTypes && p_parser.startsTypeDeclaration()) {
					nestedTypes->emplace_back(p_parser.parseNestedTypeDeclaration());
					continue;
				}
				std::optional<Token> opt_visibility = p_parser.parseVisibility();
				if (p_parser.atKeyword(KFn)) {
					throw cde::SyntaxErrors::functionInType(p_parser.peek());
				}
				if (p_parser.atKeyword(KMt)) {
					MethodDeclaration md_method = p_parser.p_functionParser.parseMethod(
					        std::move(opt_visibility), std::nullopt, std::nullopt, /*allowIntrinsic=*/false, /*allowTypeParameters=*/true);
					v_members.push_back(std::make_shared<MethodDeclaration>(std::move(md_method)));
				} else if (p_parser.atKeyword(KInit)) {
					const Token t_keyword = p_parser.advance();
					v_members.push_back(std::make_shared<StateInitializerDeclaration>(t_keyword, p_parser.p_functionParser.parseBlockBody(),
					                                                                  std::move(opt_visibility)));
				} else {
					throw cde::SyntaxErrors::expected(std::format("an '{}' block or a '{}' declaration", toString(KInit), toString(KMt)),
					                                  p_parser.peek());
				}
			}
		} catch (...) {
			p_parser.b_inStateBody = b_previousStateBody;
			throw;
		}
		p_parser.b_inStateBody = b_previousStateBody;
		return v_members;
	});
}

ClassFieldDeclaration ClassParser::parseField (std::optional<Token> visibility) const {
	const Token   t_keyword = p_parser.advance();
	TypeReference tr_type   = parseFieldType();
	const Token   t_name    = p_parser.expect(TokenType::Identifier, "a class field name");
	p_parser.expect(OAssign);
	ExpressionPtr   ep_initializer = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
	const TokenSpan ts_span        = TokenSpan::spanThrough((visibility ? *visibility : t_keyword).getSpan(), p_parser.previous().getSpan());
	return { .keyword     = t_keyword,
		 .type        = std::move(tr_type),
		 .name        = t_name,
		 .initializer = std::move(ep_initializer),
		 .visibility  = std::move(visibility),
		 .span        = ts_span };
}

TypeReference ClassParser::parseFieldType () const {
	return p_parser.p_delimitedParser.angles([this] () -> TypeReference { return p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true); });
}

ConstructorDeclaration ClassParser::parseConstructor (const Token &owner, const std::optional<Token> &visibility) const {
	const Token                       t_name       = parseMemberName(owner, "constructor");
	std::vector<ParameterDeclaration> v_parameters = p_parser.p_functionParser.parseParameters();
	const Token                       t_colon      = p_parser.expect(OColon);
	std::vector<CallArgument>         v_arguments =
	        p_parser.p_functionParser.inBody([this] () -> std::vector<CallArgument> { return p_parser.p_expressionParser.parseArgumentList(); });
	ConstructorDelegation cd_delegation(t_name, std::move(v_arguments), TokenSpan::spanThrough(t_colon.getSpan(), p_parser.previous().getSpan()));
	return { t_name, std::move(v_parameters), std::move(cd_delegation), p_parser.p_functionParser.parseBlockBody(), visibility };
}

DestructorDeclaration ClassParser::parseDestructor (const Token &owner, const std::optional<Token> &visibility) const {
	const Token t_operator = p_parser.advance();
	const Token t_name     = parseMemberName(owner, "destructor");
	return { t_operator, t_name, p_parser.p_functionParser.parseBlockBody(), visibility };
}

Token ClassParser::parseMemberName (const Token &owner, std::string_view kind) const {
	if (const Token t_name = p_parser.peek(); t_name.getType() != TokenType::Identifier || t_name.getNameKey() != owner.getNameKey()) {
		throw cde::SyntaxErrors::expected(std::format("'{}' as the {} name", owner.getLexeme(), kind), t_name);
	}
	return p_parser.advance();
}

bool ClassParser::at (const Operator op) const {
	return p_parser.peek().getLexeme() == toString(op);
}

}
