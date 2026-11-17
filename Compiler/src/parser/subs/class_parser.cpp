// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <parser/decl/class_declaration.hpp>
#include <parser/decl/declarations.hpp>
#include <parser/decl/state_declaration.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <parser/expr/class_expression.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/parser.hpp>
#include <parser/subs/class_parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <parser/type/type_parameters.hpp>
#include <parser/type/type_reference.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

TopLevelStateDeclaration ClassParser::parseTopLevelState (const Token &typedefKeyword, const Token &keyword, TypeReference payload) {
	p_parser.skipNewLines();
	std::optional<Token> opt_visibility = parseVisibility();
	const Token          t_name         = p_parser.expect(TokenType::Identifier, "a state name");
	if (!p_parser.atKeyword(Keyword::k_FOR)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' before the state host contract", cloth::to_string(Keyword::k_FOR)), p_parser.peek());
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

ClassDeclaration ClassParser::parse (const Token &keyword, std::optional<Token> visibility, std::optional<Token> finalKeyword) {
	const Token                            t_name             = p_parser.expect(TokenType::Identifier, "a class name");
	std::optional<TypeParameterList>       opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
	std::vector<ClassParameterDeclaration> v_parameters;
	if (at(Operator::o_PAREN_OPEN)) {
		v_parameters = p_parser.p_delimitedParser.parentheses([this] () -> std::vector<ClassParameterDeclaration> {
			return p_parser.p_delimitedParser.commaSeparated(Operator::o_PAREN_CLOSE,
			                                                 [this] () -> ClassParameterDeclaration { return parseHeaderParameter(); });
		});
	}
	std::vector<ClassParentClause> v_parents = p_parser.p_typeParser.parseParents(
	        "a class or interface parent name", [this] (TypeReference type, const Token &separator) -> ClassParentClause {
		        std::optional<std::vector<CallArgument>> opt_arguments;
		        if (at(Operator::o_PAREN_OPEN)) {
			        opt_arguments = p_parser.p_functionParser.inBody(
			                [this] () -> std::vector<CallArgument> { return p_parser.p_expressionParser.parseArgumentList(); });
		        }
		        return { std::move(type), std::move(opt_arguments), TokenSpan::span_through(separator.get_span(), p_parser.previous().get_span()) };
	        });
	const int i_headerEnd = p_parser.i_currentIndex;
	p_parser.skipNewLines();
	std::vector<TypeDeclarationPtr> v_nestedTypes;
	std::vector<ClassMember>        v_members;
	if (at(Operator::o_BRACE_OPEN)) {
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

ClassParameterDeclaration ClassParser::parseHeaderParameter () {
	std::optional<Token> opt_visibility = parseVisibility();
	if (!p_parser.atKeyword(Keyword::k_DEF)) {
		if (opt_visibility) {
			throw cde::SyntaxErrors::expected(std::format("'{}' after '{}' in a class header", cloth::to_string(Keyword::k_DEF),
			                                         cloth::to_string(Keyword::k_PUBLIC)),
			                             p_parser.peek());
		}
		return { p_parser.p_functionParser.parseParameter() };
	}
	const Token          t_keyword = p_parser.advance();
	TypeReference        tr_type   = parseFieldType();
	const Token          t_name    = p_parser.expect(TokenType::Identifier, "a field parameter name");
	std::optional<Token> opt_delimiter;
	ExpressionPtr        ep_fallback;
	if (at(Operator::o_ELVIS)) {
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
	return { std::move(pd_parameter), t_keyword, std::move(opt_visibility) };
}

std::vector<ClassMember> ClassParser::parseMembers (const Token &owner, std::vector<TypeDeclarationPtr> &nestedTypes) {
	std::vector<ClassMember> v_members;
	p_parser.skipNewLines();
	while (!at(Operator::o_BRACE_CLOSE)) {
		if (p_parser.startsTypeDeclaration()) {
			nestedTypes.emplace_back(p_parser.parseNestedTypeDeclaration());
			continue;
		}
		v_members.push_back(parseMember(owner));
		const ClassMember         &cm_member  = v_members.back();
		const MethodDeclaration   *md_method  = std::get_if<MethodDeclaration>(&cm_member.value);
		const StateClassMemberPtr *scmp_state = std::get_if<StateClassMemberPtr>(&cm_member.value);
		const bool b_allowsTerminator = std::holds_alternative<ClassFieldDeclaration>(cm_member.value) || (md_method && md_method->body.get_equals()) ||
		                                (scmp_state && (dynamic_cast<const StateMethodContractDeclaration *>(scmp_state->get()) ||
		                                                dynamic_cast<const StateAttachmentDeclaration *>(scmp_state->get())));
		finishMember(b_allowsTerminator, Keyword::k_CLASS);
	}
	return v_members;
}

ClassMember ClassParser::parseMember (const Token &owner) {
	std::optional<InitialStatePrefix> opt_initial;
	std::optional<Keyword>            opt_methodPrefix;
	if (p_parser.atKeyword(Keyword::k_DEFAULT)) {
		opt_initial = parseInitialState();
	} else if (p_parser.atKeyword(Keyword::k_OVERRIDE)) {
		opt_methodPrefix = Keyword::k_OVERRIDE;
	} else if (p_parser.atKeyword(Keyword::k_VIRTUAL)) {
		opt_methodPrefix = Keyword::k_VIRTUAL;
	}
	std::optional<TokenSpan> opt_prefixSpan;
	if (opt_methodPrefix) {
		const Token t_prefix = p_parser.advance();
		opt_prefixSpan       = TokenSpan::span_through(t_prefix.get_span(), p_parser.expect(Operator::o_COLON).get_span());
		p_parser.skipNewLines();
	}
	std::optional<Token> opt_visibility = parseVisibility();
	if (opt_initial && !p_parser.atKeyword(Keyword::k_STATE)) {
		throw cde::SyntaxErrors::expected(std::format("a state declaration or attachment after '{}{}'", cloth::to_string(Keyword::k_DEFAULT),
		                                         cloth::to_string(Operator::o_COLON)),
		                             p_parser.peek());
	}
	if (opt_methodPrefix && !p_parser.atKeyword(Keyword::k_MT)) {
		throw cde::SyntaxErrors::expected(std::format("a '{}' declaration after '{}{}'", cloth::to_string(Keyword::k_MT),
		                                         cloth::to_string(*opt_methodPrefix), cloth::to_string(Operator::o_COLON)),
		                             p_parser.peek());
	}
	if (p_parser.atKeyword(Keyword::k_FN)) {
		throw cde::SyntaxErrors::functionInType(p_parser.peek());
	}
	if (p_parser.atKeyword(Keyword::k_DEF)) {
		return { parseField(std::move(opt_visibility)) };
	}
	if (p_parser.atKeyword(Keyword::k_MT)) {
		return { p_parser.p_functionParser.parseMethod(std::move(opt_visibility),
			                                       opt_methodPrefix == Keyword::k_OVERRIDE ? opt_prefixSpan : std::nullopt,
			                                       opt_methodPrefix == Keyword::k_VIRTUAL ? opt_prefixSpan : std::nullopt,
			                                       /*allowIntrinsic=*/true, /*allowTypeParameters=*/true) };
	}
	if (p_parser.atKeyword(Keyword::k_OPERATOR)) {
		return { p_parser.p_functionParser.parseOperator(std::move(opt_visibility)) };
	}
	if (p_parser.atKeyword(Keyword::k_STATE)) {
		return { parseState(std::move(opt_visibility), std::move(opt_initial)) };
	}
	if (!opt_visibility && at(Operator::o_TILDE)) {
		return { parseDestructor(owner) };
	}
	if (!opt_visibility && p_parser.peek().get_type() == TokenType::Identifier) {
		return { parseConstructor(owner) };
	}
	throw cde::SyntaxErrors::expected("a class member", p_parser.peek());
}

InitialStatePrefix ClassParser::parseInitialState () {
	const Token t_keyword = p_parser.advance();
	const Token t_colon   = p_parser.expect(Operator::o_COLON);
	p_parser.skipNewLines();
	ExpressionPtr ep_payload;
	if (at(Operator::o_PAREN_OPEN)) {
		ep_payload = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr {
			return p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr {
				p_parser.skipNewLines();
				ExpressionPtr ep_expression = p_parser.p_expressionParser.parseExpression();
				p_parser.skipNewLines();
				return ep_expression;
			});
		});
	}
	const Token t_end = ep_payload ? p_parser.previous() : t_colon;
	p_parser.skipNewLines();
	return { t_keyword, std::move(ep_payload), TokenSpan::span_through(t_keyword.get_span(), t_end.get_span()) };
}

StateClassMemberPtr ClassParser::parseState (std::optional<Token> visibility, std::optional<InitialStatePrefix> initial) {
	const Token     t_keyword   = p_parser.advance();
	const Token     t_typeStart = p_parser.peek();
	const TokenSpan ts_start    = initial ? initial->span : (visibility ? *visibility : t_keyword).get_span();
	if (t_typeStart.get_type() == TokenType::Identifier) {
		if (visibility) {
			throw cde::SyntaxErrors::expected(std::format("an explicit payload type or state-method signature after '{} {}'",
			                                         cloth::to_string(Keyword::k_PUBLIC), cloth::to_string(Keyword::k_STATE)),
			                             t_typeStart);
		}
		const Token t_name = p_parser.advance();
		return std::make_shared<StateAttachmentDeclaration>(t_keyword, t_name, std::move(initial),
		                                                    TokenSpan::span_through(ts_start, t_name.get_span()));
	}
	std::vector<TypeReference> v_types = p_parser.parseTypeList();
	if (at(Operator::o_COLON_COLON)) {
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
		throw cde::SyntaxErrors::expected(std::format("one payload type in a nested '{}' declaration", cloth::to_string(Keyword::k_STATE)), t_unexpected);
	}
	const Token t_name = p_parser.expect(TokenType::Identifier, "a state name");
	p_parser.skipNewLines();
	std::vector<StateMemberPtr> v_members = parseStateBody();
	return std::make_shared<NestedStateDeclaration>(t_keyword, std::move(visibility), t_name, std::move(v_types.front()), std::move(initial),
	                                                std::move(v_members), TokenSpan::span_through(ts_start, p_parser.previous().get_span()));
}

std::vector<StateMemberPtr> ClassParser::parseStateBody (std::vector<TypeDeclarationPtr> *nestedTypes) {
	return p_parser.p_delimitedParser.braces([this, nestedTypes] () -> std::vector<StateMemberPtr> {
		std::vector<StateMemberPtr> v_members;
		const bool                  b_previousStateBody = std::exchange(p_parser.b_inStateBody, true);
		try {
			p_parser.skipNewLines();
			while (!at(Operator::o_BRACE_CLOSE)) {
				if (nestedTypes && p_parser.startsTypeDeclaration()) {
					nestedTypes->emplace_back(p_parser.parseNestedTypeDeclaration());
					continue;
				}
				std::optional<Token> opt_visibility = parseVisibility();
				if (p_parser.atKeyword(Keyword::k_FN)) {
					throw cde::SyntaxErrors::functionInType(p_parser.peek());
				}
				bool b_allowsTerminator = false;
				if (p_parser.atKeyword(Keyword::k_MT)) {
					MethodDeclaration md_method = p_parser.p_functionParser.parseMethod(
					        std::move(opt_visibility), std::nullopt, std::nullopt, /*allowIntrinsic=*/false, /*allowTypeParameters=*/true);
					b_allowsTerminator = md_method.body.get_equals() != nullptr;
					v_members.push_back(std::make_shared<MethodDeclaration>(std::move(md_method)));
				} else if (!opt_visibility && p_parser.atKeyword(Keyword::k_INIT)) {
					const Token t_keyword = p_parser.advance();
					v_members.push_back(
					        std::make_shared<StateInitializerDeclaration>(t_keyword, p_parser.p_functionParser.parseBlockBody()));
				} else {
					throw cde::SyntaxErrors::expected(std::format("an '{}' block or a '{}' declaration", cloth::to_string(Keyword::k_INIT),
					                                         cloth::to_string(Keyword::k_MT)),
					                             p_parser.peek());
				}
				finishMember(b_allowsTerminator, Keyword::k_STATE);
			}
		} catch (...) {
			p_parser.b_inStateBody = b_previousStateBody;
			throw;
		}
		p_parser.b_inStateBody = b_previousStateBody;
		return v_members;
	});
}

void ClassParser::finishMember (bool allowsTerminator, Keyword kind) {
	const Token t_next = p_parser.peek();
	if (allowsTerminator && p_parser.isSemiColon(t_next)) {
		p_parser.advance();
	} else if (t_next.get_lexeme() != cloth::to_string(Operator::o_BRACE_CLOSE) && t_next.get_type() != TokenType::NewLine) {
		if (allowsTerminator) {
			throw cde::SyntaxErrors::newline(t_next);
		}
		throw cde::SyntaxErrors::expected(std::format("a newline or '{}' after a brace-bodied {} member", cloth::to_string(Operator::o_BRACE_CLOSE),
		                                         cloth::to_string(kind)),
		                             t_next);
	}
	p_parser.skipNewLines();
}

ClassFieldDeclaration ClassParser::parseField (std::optional<Token> visibility) {
	const Token   t_keyword = p_parser.advance();
	TypeReference tr_type   = parseFieldType();
	const Token   t_name    = p_parser.expect(TokenType::Identifier, "a class field name");
	p_parser.expect(Operator::o_ASSIGN);
	ExpressionPtr   ep_initializer = p_parser.p_functionParser.inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
	const TokenSpan ts_span        = TokenSpan::span_through((visibility ? *visibility : t_keyword).get_span(), p_parser.previous().get_span());
	return { t_keyword, std::move(tr_type), t_name, std::move(ep_initializer), std::move(visibility), ts_span };
}

TypeReference ClassParser::parseFieldType () {
	return p_parser.p_delimitedParser.angles([this] () -> TypeReference {
		p_parser.skipNewLines();
		TypeReference tr_type = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
		p_parser.skipNewLines();
		return tr_type;
	});
}

ConstructorDeclaration ClassParser::parseConstructor (const Token &owner) {
	const Token                       t_name       = parseMemberName(owner, "constructor");
	std::vector<ParameterDeclaration> v_parameters = p_parser.p_functionParser.parseParameters();
	const Token                       t_colon      = p_parser.expect(Operator::o_COLON);
	std::vector<CallArgument>         v_arguments =
	        p_parser.p_functionParser.inBody([this] () -> std::vector<CallArgument> { return p_parser.p_expressionParser.parseArgumentList(); });
	ConstructorDelegation cd_delegation(t_name, std::move(v_arguments), TokenSpan::span_through(t_colon.get_span(), p_parser.previous().get_span()));
	return { t_name, std::move(v_parameters), std::move(cd_delegation), p_parser.p_functionParser.parseBlockBody() };
}

DestructorDeclaration ClassParser::parseDestructor (const Token &owner) {
	const Token t_operator = p_parser.advance();
	const Token t_name     = parseMemberName(owner, "destructor");
	return { t_operator, t_name, p_parser.p_functionParser.parseBlockBody() };
}

Token ClassParser::parseMemberName (const Token &owner, std::string_view kind) {
	const Token t_name = p_parser.peek();
	if (t_name.get_type() != TokenType::Identifier || t_name.get_lexeme() != owner.get_lexeme()) {
		throw cde::SyntaxErrors::expected(std::format("'{}' as the {} name", owner.get_lexeme(), kind), t_name);
	}
	return p_parser.advance();
}

std::optional<Token> ClassParser::parseVisibility () {
	return p_parser.atKeyword(Keyword::k_PUBLIC) ? std::make_optional(p_parser.advance()) : std::nullopt;
}

bool ClassParser::at (Operator op) {
	return p_parser.peek().get_lexeme() == cloth::to_string(op);
}

}
