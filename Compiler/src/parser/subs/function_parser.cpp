// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Calls.rst

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {
namespace {

bool isVoidReturn (const std::vector<TypeReference> &returnTypes) {
	return returnTypes.empty() || (returnTypes.size() == 1 && returnTypes.front().getSpelling() == toString(KVoid));
}

bool endsInStatement (const std::vector<StatementPtr> &statements);

bool isStatementForm (const IfExpression &expression) {
	if (!expression.otherwise) {
		return true;
	}
	return std::ranges::any_of(expression.branches, [] (const ConditionalBranch &branch) -> bool { return endsInStatement(branch.statements); }) ||
	       endsInStatement(*expression.otherwise);
}

bool endsInStatement (const std::vector<StatementPtr> &statements) {
	if (statements.empty()) {
		return true;
	}
	const Statement *stmt_last = statements.back().get();
	if (const auto *es_last = dynamic_cast<const ExpressionStatement *>(stmt_last)) {
		const auto *ie_conditional = dynamic_cast<const IfExpression *>(es_last->expression.get());
		return ie_conditional && isStatementForm(*ie_conditional);
	}
	// An explicit return already satisfies a value branch.
	return dynamic_cast<const ReturnStatement *>(stmt_last) == nullptr;
}

}

FunctionDeclaration FunctionParser::parseFunction () {
	std::optional<Token> opt_visibility = p_parser.parseVisibility();
	if (!p_parser.atKeyword(KFn)) {
		throw cde::SyntaxErrors::expected(std::format("a '{}' declaration", toString(KFn)), p_parser.peek());
	}
	const Token                       t_keyword          = p_parser.advance();
	std::vector<TypeReference>        v_returnTypes      = p_parser.parseTypeList();
	const Token                       t_name             = p_parser.expect(TokenType::Identifier, "a function name");
	std::optional<TypeParameterList>  opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
	std::vector<ParameterDeclaration> v_parameters       = parseParameters();
	FunctionBody                      fb_body            = parseBody("function", isVoidReturn(v_returnTypes), /*allowIntrinsic=*/true);
	return { t_keyword,
		 t_name,
		 std::move(v_returnTypes),
		 std::move(v_parameters),
		 std::move(fb_body),
		 std::move(opt_visibility),
		 std::move(opt_typeParameters) };
}

std::vector<ParameterDeclaration> FunctionParser::parseParameters () {
	return p_parser.p_delimitedParser.parentheses([this] () -> std::vector<ParameterDeclaration> {
		return p_parser.p_delimitedParser.commaSeparated(OParenClose, [this] () -> ParameterDeclaration { return parseParameter(); });
	});
}

ParameterDeclaration FunctionParser::parseParameter () {
	std::optional<Token> opt_finalKeyword    = p_parser.atKeyword(KFinal) ? std::make_optional(p_parser.advance()) : std::nullopt;
	std::optional<Token> opt_optionalKeyword = p_parser.atKeyword(KOpt) ? std::make_optional(p_parser.advance()) : std::nullopt;
	std::optional<Token> opt_opening;
	if (opt_optionalKeyword) {
		opt_opening = p_parser.expect(OLess);
		p_parser.skipNewLines();
	}
	TypeReference        tr_type = p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
	std::optional<Token> opt_closing;
	if (opt_optionalKeyword) {
		p_parser.skipNewLines();
		opt_closing = p_parser.expect(OGreater);
	}
	const Token          t_name = p_parser.expect(TokenType::Identifier, "a parameter name");
	std::optional<Token> opt_delimiter;
	ExpressionPtr        ep_fallback;
	if (opt_optionalKeyword) {
		opt_delimiter = p_parser.expect(OElvis);
		ep_fallback   = inBody([this] () -> ExpressionPtr { return p_parser.p_expressionParser.parseExpression(); });
	}
	const TokenSpan ts_start = opt_finalKeyword ? opt_finalKeyword->getSpan() : opt_optionalKeyword ? opt_optionalKeyword->getSpan() : tr_type.getSpan();
	const TokenSpan ts_span  = TokenSpan::spanThrough(ts_start, p_parser.previous().getSpan());
	return {
		.type             = std::move(tr_type),
		.name             = t_name,
		.span             = ts_span,
		.finalKeyword     = std::move(opt_finalKeyword),
		.optKeyword       = std::move(opt_optionalKeyword),
		.defaultDelimiter = std::move(opt_delimiter),
		.defaultValue     = std::move(ep_fallback),
		.optOpening       = std::move(opt_opening),
		.optClosing       = std::move(opt_closing),
	};
}

FunctionParser::MethodHeader FunctionParser::methodHeader (const bool allowTypeParameters) {
	const Token                t_keyword     = p_parser.advance();
	std::vector<TypeReference> v_returnTypes = p_parser.parseTypeList();
	const Token                t_name        = p_parser.expect(TokenType::Identifier, "a method name");
	if (!allowTypeParameters && at(OLess)) {
		throw cde::GenericErrors::syntax("Method type parameters", p_parser.peek());
	}
	std::optional<TypeParameterList> opt_typeParameters = p_parser.p_typeParser.parseTypeParameters();
	return { .keyword        = t_keyword,
		 .name           = t_name,
		 .returnTypes    = std::move(v_returnTypes),
		 .parameters     = parseParameters(),
		 .typeParameters = std::move(opt_typeParameters) };
}

MethodDeclaration FunctionParser::parseMethod (std::optional<Token> visibility, const std::optional<TokenSpan> &overridePrefix,
                                               const std::optional<TokenSpan> &virtualPrefix, bool allowIntrinsic, bool allowTypeParameters) {
	auto [keyword, name, returnTypes, parameters, typeParameters] = methodHeader(allowTypeParameters);
	FunctionBody fb_body                                          = parseBody("method", isVoidReturn(returnTypes), allowIntrinsic);
	return { keyword,      name,           std::move(returnTypes), std::move(parameters),    std::move(fb_body), std::move(visibility),
		 std::nullopt, overridePrefix, virtualPrefix,          std::move(typeParameters) };
}

InterfaceMethodDeclaration FunctionParser::parseInterfaceMethod (std::optional<Token> visibility) {
	auto [keyword, name, returnTypes, parameters, typeParameters] = methodHeader(/*allowTypeParameters=*/true);
	const int i_signatureEnd                                      = p_parser.i_currentIndex;
	p_parser.skipNewLines();
	std::optional<InterfaceMethodFallback> opt_fallback;
	if (at(OAssign)) {
		opt_fallback = parseInterfaceFallback();
	} else if (at(OBraceOpen)) {
		throw cde::SyntaxErrors::expected(std::format("'{} {}' before an interface method body", toString(OAssign), toString(KDefault)),
		                                  p_parser.peek());
	} else {
		p_parser.i_currentIndex = i_signatureEnd;
	}
	const TokenSpan ts_span = TokenSpan::spanThrough((visibility ? *visibility : keyword).getSpan(), p_parser.previous().getSpan());
	return {
		keyword, name, std::move(returnTypes), std::move(parameters), std::move(opt_fallback), ts_span, std::move(typeParameters), std::move(visibility)
	};
}

InterfaceMethodFallback FunctionParser::parseInterfaceFallback () {
	const Token t_equals = p_parser.advance();
	if (!p_parser.atKeyword(KDefault)) {
		throw cde::SyntaxErrors::expected(std::format("'{}' after '{}' in an interface method", toString(KDefault), toString(OAssign)),
		                                  p_parser.peek());
	}
	const Token t_keyword = p_parser.advance();
	p_parser.skipNewLines();
	if (at(OParenOpen)) {
		ExpressionPtr   ep_expression = inBody([this] () -> ExpressionPtr {
                        return p_parser.p_delimitedParser.parentheses([this] () -> ExpressionPtr {
                                p_parser.skipNewLines();
                                ExpressionPtr ep_results =
                                        p_parser.parseResults(nullptr, p_parser.peek().getSpan(), /*allowWhileTrailer=*/false, /*multiline=*/true);
                                p_parser.skipNewLines();
                                return ep_results;
                        });
                });
		const TokenSpan ts_span       = TokenSpan::spanThrough(t_equals.getSpan(), p_parser.previous().getSpan());
		return { .equals  = t_equals,
			 .keyword = t_keyword,
			 .body    = { ExpressionBody{ .equals = t_equals, .expression = std::move(ep_expression), .span = ts_span } } };
	}
	if (at(OBraceOpen)) {
		return { .equals = t_equals, .keyword = t_keyword, .body = { parseBlockBody() } };
	}
	throw cde::SyntaxErrors::expected(std::format("a parenthesized expression or a block after '{}'", toString(KDefault)), p_parser.peek());
}

MethodDeclaration FunctionParser::parseOperator (std::optional<Token> visibility) {
	const Token       t_keyword     = p_parser.advance();
	const bool        b_hasSelector = at(OLess) && !at(OParenOpen, 1);
	const RestoreFlag rf_width(p_parser.b_allowInferredOperatorWidth, false);
	p_parser.b_allowInferredOperatorWidth = b_hasSelector;

	std::optional<TypeReference> opt_selector;
	if (b_hasSelector) {
		opt_selector = p_parser.p_delimitedParser.angles(
		        [this] () -> TypeReference { return p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true); });
	}
	p_parser.b_allowInferredOperatorWidth = opt_selector && opt_selector->capturesWidth();
	const bool b_provider                 = at(OColonColon);
	if (b_provider) {
		p_parser.advance();
	}
	const Token t_name          = b_provider ? p_parser.expect(TokenType::Identifier, "an operator provider name") : parseOperatorName();
	bool        b_rightReceiver = false;
	using OptionalParameters    = std::vector<std::optional<ParameterDeclaration>>;
	OptionalParameters                v_parsedParameters = p_parser.p_delimitedParser.parentheses([this, &b_rightReceiver] () -> OptionalParameters {
                return p_parser.p_delimitedParser.commaSeparated(OParenClose, [this, &b_rightReceiver] () -> std::optional<ParameterDeclaration> {
                        if (!p_parser.atKeyword(KSelf)) {
                                return parseParameter();
                        }
                        p_parser.advance();
                        b_rightReceiver = true;
                        p_parser.skipNewLines();
                        if (!at(OParenClose)) {
                                throw cde::SyntaxErrors::expected(std::format("'{}' after '{}'", toString(OParenClose), toString(KSelf)), p_parser.peek());
                        }
                        return std::nullopt;
                });
        });
	std::vector<ParameterDeclaration> v_parameters;
	v_parameters.reserve(v_parsedParameters.size());
	for (std::optional<ParameterDeclaration> &opt_parameter : v_parsedParameters) {
		if (opt_parameter) {
			v_parameters.push_back(std::move(*opt_parameter));
		}
	}

	std::vector<TypeReference> v_returnTypes;
	if (at(OPointerRight)) {
		p_parser.advance();
		v_returnTypes.push_back(p_parser.p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
	}
	const Operator o_operation      = t_name.getOperator();
	const bool     b_allowStatement = o_operation == OIndexWrite || isUpdate(o_operation);
	FunctionBody   fb_body          = parseBody(toString(KOperator), b_allowStatement);
	return { t_keyword,
		 t_name,
		 std::move(v_returnTypes),
		 std::move(v_parameters),
		 std::move(fb_body),
		 std::move(visibility),
		 OperatorMember{ .selector = std::move(opt_selector), .rightReceiver = b_rightReceiver, .provider = b_provider } };
}

Token FunctionParser::parseOperatorName () const {
	const Token t_token = p_parser.peek();
	if (t_token.getType() == TokenType::Identifier && t_token.getLexeme() == toString(OForeach)) {
		p_parser.advance();
		return { t_token.getLexeme(), OForeach, t_token.getSpan() };
	}
	if (t_token.getType() != TokenType::Operator && !p_parser.atKeyword(KIs)) {
		throw cde::SyntaxErrors::expected("an operator", t_token);
	}
	p_parser.advance();
	if (t_token.getLexeme() != toString(OBracketOpen)) {
		return t_token;
	}

	const Token t_closing   = p_parser.expect(OBracketClose);
	Operator    o_operation = OIndexRead;
	if (at(OQuestion)) {
		o_operation = OIndexValidate;
	} else if (at(OAssign)) {
		o_operation = OIndexWrite;
	}
	const Token t_end = o_operation == OIndexRead ? t_closing : p_parser.advance();
	return { std::string(toString(o_operation)), o_operation, TokenSpan::spanThrough(t_token.getSpan(), t_end.getSpan()) };
}

BlockBody FunctionParser::parseBlockBody () {
	p_parser.skipNewLines();
	return inBody([this] () -> BlockBody { return p_parser.parseBlock(); });
}

FunctionBody FunctionParser::parseBody (std::string_view kind, bool allowStatement, bool allowIntrinsic) {
	p_parser.skipNewLines();
	FunctionBody body = inBody([this, kind, allowStatement, allowIntrinsic] () -> FunctionBody {
		if (at(OBraceOpen)) {
			return { p_parser.parseBlock() };
		}
		if (!at(OAssign)) {
			throw cde::SyntaxErrors::expected(std::format("a {} body ('{}' or '{}')", kind, toString(OBraceOpen), toString(OAssign)),
			                                  p_parser.peek());
		}
		const Token t_equals = p_parser.advance();
		const Token t_next   = p_parser.peek();
		if (allowIntrinsic && t_next.getType() == TokenType::Identifier && t_next.getLexeme() == toString(KIntrinsic) &&
		    p_parser.peek(1).getType() == TokenType::StringLiteral) {
			p_parser.advance();
			return { IntrinsicBody(t_equals, p_parser.advance()) };
		}
		if (allowStatement) {
			return parseVoidBody(t_equals);
		}
		ExpressionPtr   ep_expression = p_parser.parseResults(nullptr, t_next.getSpan(), /*allowWhileTrailer=*/false, /*multiline=*/false);
		const TokenSpan ts_span       = TokenSpan::spanThrough(t_equals.getSpan(), p_parser.previous().getSpan());
		return { ExpressionBody{ .equals = t_equals, .expression = std::move(ep_expression), .span = ts_span } };
	});
	if (p_parser.explicitSemicolons() && body.getEquals() && !(allowStatement && Parser::isSemiColon(p_parser.previous()))) {
		p_parser.requireSemicolon(kind);
	}
	return body;
}

FunctionBody FunctionParser::parseVoidBody (const Token &equals) const {
	const bool                 b_startsWithIf = p_parser.atKeyword(KIf);
	BlockBody                  bb_body        = p_parser.parseUnbracedBody();
	const TokenSpan            ts_span        = TokenSpan::spanThrough(equals.getSpan(), bb_body.span);
	const ExpressionStatement *es_statement = bb_body.statements.size() == 1 ? dynamic_cast<const ExpressionStatement *>(bb_body.statements.front().get()) :
	                                                                           nullptr;
	ExpressionPtr              ep_expression = es_statement ? es_statement->expression : nullptr;
	if (const IfExpression *ie_conditional = b_startsWithIf ? dynamic_cast<const IfExpression *>(ep_expression.get()) : nullptr;
	    !ep_expression || (ie_conditional && isStatementForm(*ie_conditional))) {
		bb_body.span   = ts_span;
		bb_body.equals = equals;
		return { std::move(bb_body) };
	}
	return { ExpressionBody{ .equals = equals, .expression = std::move(ep_expression), .span = ts_span } };
}

bool &FunctionParser::functionBodyFlag () const {
	return p_parser.b_inFnBody;
}

bool FunctionParser::at (const Operator op, const int offset) const {
	return p_parser.peek(offset).getLexeme() == toString(op);
}

}
