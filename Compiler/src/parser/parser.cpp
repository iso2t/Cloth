// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/statements/ControlFlow.rst
// See Documentation/language/source/Visibility.rst

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/import_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/expr/provider_expressions.hpp>
#include <ast/expr/struct_expressions.hpp>
#include <ast/source_unit.hpp>
#include <ast/stmt/do_while_statement.hpp>
#include <ast/stmt/for_statement.hpp>
#include <ast/stmt/loop_control.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/stmt/while_statement.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <parser/parser.hpp>
#include <token/token.hpp>

namespace cloth {

const std::unordered_set<std::string_view> Parser::ASSIGNMENT_OPERATORS{
	toString(OAssign),   toString(OFinalAssign), toString(OPlusEqual), toString(OMinusEqual), toString(OMulEqual),
	toString(ODivEqual), toString(OModEqual),    toString(OPlusPlus),  toString(OMinusMinus),
};

namespace {

const std::unordered_set UPDATE_OPERATORS{
	toString(OPlusPlus),
	toString(OMinusMinus),
};

template <typename Value> class ScopedValue {
    public:
	ScopedValue (Value &target, Value current) : val_target(target), val_previous(std::exchange(target, current)) {
	}

	~ScopedValue () {
		val_target = val_previous;
	}

	ScopedValue (const ScopedValue &)           = delete;
	ScopedValue &operator=(const ScopedValue &) = delete;

    private:
	Value &val_target;
	Value  val_previous;
};

bool isBlockEnd (const Token &token) {
	return token.getType() == TokenType::EndOfFile || token.getLexeme() == toString(OBraceClose);
}

}

Parser::Parser (std::vector<Token> tokens) : v_tokens(std::move(tokens)) {
	std::erase_if(v_tokens, [] (const Token &token) { return token.getType() == TokenType::NewLine; });
}

Token Parser::requireSemicolon (const std::string_view after) {
	if (!at(OSemicolon)) {
		throw cde::SyntaxErrors::missingSemicolon(previous().getSpan(), after);
	}
	return advance();
}

bool Parser::consumeEmptyTypeHeader (const std::string_view after) {
	if (at(OBraceOpen)) {
		return false;
	}
	if (!at(OSemicolon)) {
		throw cde::SyntaxErrors::missingSemicolon(previous().getSpan(), after, /*allowBody=*/true);
	}
	advance();
	return true;
}

Token Parser::peek (const int offset) {
	if (v_tokens.empty()) {
		throw std::out_of_range("Token stream is empty.");
	}

	const std::int64_t i64_target = static_cast<std::int64_t>(i_currentIndex) + offset;
	if (i64_target < 0 || static_cast<std::uint64_t>(i64_target) >= v_tokens.size()) {
		return v_tokens.back();
	}
	return v_tokens[static_cast<std::size_t>(i64_target)];
}

Token Parser::previous () {
	return v_tokens.at(static_cast<std::size_t>(i_currentIndex - 1));
}

Token Parser::advance () {
	Token const t_token = v_tokens.at(static_cast<std::size_t>(i_currentIndex));
	++i_currentIndex;
	return t_token;
}

SourceUnit Parser::parse () {
	return parseDeclarations();
}

SourceUnit Parser::parseDeclarations () {
	std::vector<DeclarationPtr>    v_declarations;
	std::vector<ImportDeclaration> v_imports;
	while (atKeyword(KUsing)) {
		v_imports.push_back(p_importParser.parse());
	}
	parseDeclarationItems(/*braced=*/false, [&] (const PublicBlockOrigin &publicBlock) {
		if (atKeyword(KUsing)) {
			throw cde::ImportErrors::outsideHeader(peek());
		}
		rejectMethodOutsideType();
		std::shared_ptr<Declaration> dp_declaration = startsTypeDeclaration() ? parseTypeDeclaration() :
		                                                                        std::make_shared<FunctionDeclaration>(p_functionParser.parseFunction());
		dp_declaration->publicBlock                 = publicBlock;
		v_declarations.push_back(std::move(dp_declaration));
	});
	return { .declarations = std::move(v_declarations), .end = peek().getSpan(), .imports = std::move(v_imports) };
}

void Parser::parseDeclarationItems (const bool braced, const std::function<void(const PublicBlockOrigin &)> &parseItem) {
	// Each file or type starts a fresh list. Only braces opened here supply visibility.
	std::vector<PublicBlockOrigin> groups{ nullptr };
	while (peek().getType() != TokenType::EndOfFile) {
		if (atKeyword(KPublic) && at(OBraceOpen, 1)) {
			groups.push_back(std::make_shared<TokenSpan>(advance().getSpan()));
			advance();
		} else if (at(OBraceClose) && groups.back()) {
			advance();
			groups.pop_back();
		} else if (braced && at(OBraceClose)) {
			return;
		} else {
			parseItem(groups.back());
		}
	}
	if (groups.back()) expect(OBraceClose);
}

bool Parser::startsTypeDeclaration () {
	int i_offset = atKeyword(KPublic) ? 1 : 0;
	if (atKeyword(KFinal, i_offset)) {
		++i_offset;
	}
	return atKeyword(KTypeDef, i_offset);
}

TypeDeclarationPtr Parser::parseNestedTypeDeclaration (const PublicBlockOrigin &publicBlock) {
	const ScopedValue sv_stateBody(b_inStateBody, false);
	auto              tdp_type = std::dynamic_pointer_cast<TypeDeclaration>(parseTypeDeclaration(/*nested=*/true));
	if (!tdp_type) {
		throw std::bad_cast();
	}
	tdp_type->publicBlock = publicBlock;
	return tdp_type;
}

std::shared_ptr<Declaration> Parser::parseTypeDeclaration (bool nested) {
	std::optional<Token>         opt_visibility   = parseVisibility();
	std::optional<Token>         opt_finalKeyword = atKeyword(KFinal) ? std::make_optional(advance()) : std::nullopt;
	const Token                  t_keyword        = advance();
	std::optional<TypeReference> opt_payload;
	std::optional<IntrinsicBinding> opt_intrinsic;
	const Token                     t_kind = p_delimitedParser.angles([&] () -> Token {
		const Token t_start = peek();
                if (atKeyword(KClass)) {
                        return advance();
                }
                if (opt_finalKeyword) {
                        throw cde::SyntaxErrors::expected(std::format("'{}' as the type declaration kind after '{}'", toString(KClass), toString(KFinal)),
			                                                   t_start);
                }
                if (atKeyword(KStruct) || atKeyword(KEnum) || atKeyword(KInterface)) {
                        return advance();
                }
		if (!nested && t_start.getLexeme() == toString(KIntrinsic)) {
			advance();
			opt_intrinsic = IntrinsicBinding{ expect(TokenType::StringLiteral, "a primitive family ID") };
			return t_start;
		}
		if (!nested && atKeyword(KState)) {
                        const Token t_state = advance();
                        if (opt_visibility) {
                                throw cde::SyntaxErrors::expected(std::format("state visibility after the '{}' header", toString(KTypeDef)), *opt_visibility);
                        }
			opt_payload =
			        p_delimitedParser.angles([this] () -> TypeReference { return p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true); });
			return t_state;
                }
                if (nested) {
                        throw cde::SyntaxErrors::expected(std::format("'{}', '{}', '{}', or '{}' as the nested type declaration kind", toString(KStruct),
			                                                               toString(KClass), toString(KEnum), toString(KInterface)),
			                                                   t_start);
                }
		throw cde::SyntaxErrors::expected(std::format("'{}', '{}', '{}', '{}', '{}', or '{}' as the type declaration kind", toString(KStruct),
		                                              toString(KClass), toString(KEnum), toString(KInterface), toString(KState), toString(KIntrinsic)),
		                                  t_start);
	});

	const std::string &s_kind = t_kind.getLexeme();
	if (opt_intrinsic) {
		return std::make_shared<IntrinsicTypeDeclaration>(
		        p_structParser.parseIntrinsic(t_keyword, std::move(opt_visibility), std::move(*opt_intrinsic)));
	}
	if (s_kind == toString(KClass)) {
		return std::make_shared<ClassDeclaration>(p_classParser.parse(t_keyword, std::move(opt_visibility), std::move(opt_finalKeyword)));
	}
	if (s_kind == toString(KEnum)) {
		return std::make_shared<EnumDeclaration>(p_enumParser.parse(t_keyword, std::move(opt_visibility)));
	}
	if (s_kind == toString(KInterface)) {
		return std::make_shared<InterfaceDeclaration>(p_interfaceParser.parse(t_keyword, t_kind, std::move(opt_visibility)));
	}
	if (opt_payload) {
		return std::make_shared<TopLevelStateDeclaration>(p_classParser.parseTopLevelState(t_keyword, t_kind, std::move(*opt_payload)));
	}
	return std::make_shared<StructDeclaration>(p_structParser.parse(t_keyword, std::move(opt_visibility)));
}

bool Parser::atKeyword (const Keyword keyword, const int offset) {
	const Token t_offset = peek(offset);
	return t_offset.getType() == TokenType::Keyword && t_offset.getLexeme() == toString(keyword);
}

std::optional<Token> Parser::parseVisibility () {
	return atKeyword(KPublic) ? std::make_optional(advance()) : std::nullopt;
}

bool Parser::at (const Operator op, const int offset) {
	return peek(offset).getLexeme() == toString(op);
}

void Parser::rejectMethodOutsideType () {
	if (const int i_offset = atKeyword(KPublic) ? 1 : 0; atKeyword(KMt, i_offset)) {
		throw cde::SyntaxErrors::methodOutsideType(peek(i_offset));
	}
}

BlockBody Parser::parseBlock () {
	const Token               t_opening    = peek();
	std::vector<StatementPtr> v_statements = p_delimitedParser.braces([this] () -> std::vector<StatementPtr> { return parseStatements(); });
	const TokenSpan           ts_span      = TokenSpan::spanThrough(t_opening.getSpan(), previous().getSpan());
	return { .span = ts_span, .statements = std::move(v_statements) };
}

std::unique_ptr<Expression> Parser::parseBindingBrace (const bool allowLiteral, const bool allowSingleField,
                                                       const std::function<std::unique_ptr<FunctionBlockExpression>(BlockBody)> &deferred) {
	bool              hasReturn = false;
	const ScopedValue trackReturns(bindingBraceHasReturn, &hasReturn);
	if (allowLiteral) {
		const Token opening = peek();
		const int   start   = i_currentIndex;
		try {
			auto fields = p_functionParser.inBody([this] {
				return p_delimitedParser.braces([this] {
					return p_delimitedParser.commaSeparated(OBraceClose, [this] { return p_expressionParser.parseExpression(); });
				});
			});
			if (fields.size() > 1 || (allowSingleField && !hasReturn)) {
				return std::make_unique<StructLiteral>(std::move(fields), TokenSpan::spanThrough(opening.getSpan(), previous().getSpan()));
			}
		} catch (const cde::SyntaxException &) {
			// A statement or an interior semicolon selects a deferred block.
		}
		i_currentIndex = start;
	}
	return deferred(p_functionParser.inBody([this] { return parseBlock(); }));
}

std::vector<StatementPtr> Parser::parseStatements () {
	std::vector<StatementPtr> v_statements;
	while (!isBlockEnd(peek())) {
		const Token first = peek();
		if (isSemiColon(first)) {
			throw cde::SyntaxErrors::expected("a statement", first);
		}
		if (p_bindingParser.startsBinding()) {
			std::vector<StatementPtr> v_bindings = p_bindingParser.parseBindings();
			v_statements.insert(v_statements.end(), std::make_move_iterator(v_bindings.begin()), std::make_move_iterator(v_bindings.end()));
		} else {
			v_statements.push_back(parseStatement());
		}
		finishStatement(first, *v_statements.back());
	}
	return v_statements;
}

void Parser::finishStatement (const Token &first, const Statement &last) {
	if (dynamic_cast<const ForStatement *>(&last) || dynamic_cast<const WhileStatement *>(&last) || dynamic_cast<const DoWhileStatement *>(&last)) {
		return;
	}
	if (const auto *expression = dynamic_cast<const ExpressionStatement *>(&last);
	    first.getLexeme() == toString(KIf) && expression && dynamic_cast<const IfExpression *>(expression->expression.get())) {
		return;
	}
	if (dynamic_cast<const ReturnStatement *>(&last)) {
		requireSemicolon("return statement");
	} else {
		requireSemicolon("statement");
	}
}

bool Parser::isSemiColon (const Token &token) {
	return token.getType() == TokenType::Operator && token.getLexeme() == toString(OSemicolon);
}

StatementPtr Parser::parseStatement (bool allowWhileTrailer, bool singleValue) {
	if (atKeyword(KUsing)) {
		throw cde::ImportErrors::outsideHeader(peek());
	}
	rejectMethodOutsideType();
	if (!singleValue && atKeyword(KIf)) {
		ExpressionPtr expression = std::make_shared<IfExpression>(p_ifParser.parseIf(allowWhileTrailer, /*statement=*/true));
		if (!isSemiColon(previous())) {
			const TokenSpan start = expression->getSpan();
			expression            = p_expressionParser.continueFromPrimary(std::move(expression), allowWhileTrailer);
			expression            = parseResults(std::move(expression), start, allowWhileTrailer);
		}
		return std::make_shared<ExpressionStatement>(std::move(expression));
	}
	if (atKeyword(KFor)) {
		return std::make_shared<ForStatement>(p_forLoopParser.parseFor(allowWhileTrailer));
	}
	if (atKeyword(KWhile)) {
		return std::make_shared<WhileStatement>(p_whileLoopParser.parseWhile(allowWhileTrailer));
	}
	if (atKeyword(KDo)) {
		return std::make_shared<DoWhileStatement>(p_doWhileLoopParser.parseDoWhile());
	}
	if (atKeyword(KBreak) || atKeyword(KContinue)) {
		return std::make_shared<LoopJumpStatement>(advance());
	}
	if (atKeyword(KReturn)) {
		return std::make_shared<ReturnStatement>(returnStatement(allowWhileTrailer));
	}
	if (startsAssignmentList()) {
		std::vector v_targets{ ReceivingTarget::of(advance()) };
		while (at(OComma)) {
			advance();
			v_targets.push_back(parseReceivingTarget(/*assignment=*/true));
		}
		if (at(OFinalAssign)) {
			throw cde::SyntaxErrors::reassignment(peek());
		}
		expect(OAssign);
		return std::make_shared<MultiAssignmentStatement>(std::move(v_targets), parseResults(nullptr, std::nullopt, allowWhileTrailer));
	}

	const Token t_start = peek();
	if (const Token t_following = peek(1); (t_start.getType() == TokenType::Identifier || atKeyword(KSelf)) &&
	                                       t_following.getType() == TokenType::Operator && ASSIGNMENT_OPERATORS.contains(t_following.getLexeme())) {
		const Token t_name = advance();
		if (at(OFinalAssign)) {
			throw cde::SyntaxErrors::reassignment(peek());
		}
		const Token t_operator = advance();
		if (UPDATE_OPERATORS.contains(t_operator.getLexeme())) {
			return std::make_shared<UpdateStatement>(t_name, t_operator);
		}
		if (t_operator.getLexeme() == toString(OAssign)) {
			return std::make_shared<AssignmentStatement>(t_name, parseResults(nullptr, std::nullopt, allowWhileTrailer));
		}
		ExpressionPtr const ep_right = p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
		ExpressionPtr       ep_value = compoundExpression(std::make_shared<NameExpression>(t_name), t_operator, ep_right);
		return std::make_shared<AssignmentStatement>(t_name, std::move(ep_value), t_operator);
	}

	ExpressionPtr      ep_expression = p_expressionParser.parseExpression(0, /*stopAtAssignment=*/true, /*stopAtRange=*/false, allowWhileTrailer);
	const Token        t_operator    = peek();
	const std::string &s_operator    = t_operator.getLexeme();
	if (ASSIGNMENT_OPERATORS.contains(s_operator)) {
		if (const auto *pae_provider = dynamic_cast<const ProviderAccessExpression *>(ep_expression.get())) {
			throw cde::SyntaxErrors::readOnlyProvider(pae_provider->name);
		}
		if (s_operator == toString(OFinalAssign)) {
			throw cde::SyntaxErrors::reassignment(t_operator);
		}
		const bool                             b_fieldTarget = hasFieldTarget(*ep_expression);
		std::shared_ptr<const IndexExpression> iep_index     = std::dynamic_pointer_cast<const IndexExpression>(ep_expression);
		if (!b_fieldTarget && !iep_index) {
			throw cde::SyntaxErrors::expected("an indexed array assignment target", t_operator);
		}
		advance();
		ExpressionPtr ep_value;
		if (s_operator == toString(OAssign)) {
			ep_value = parseResults(nullptr, std::nullopt, allowWhileTrailer);
		} else if (!UPDATE_OPERATORS.contains(s_operator)) {
			ep_value = p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
		}
		if (b_fieldTarget) {
			return std::make_shared<FieldWriteStatement>(std::move(ep_expression), t_operator, std::move(ep_value));
		}
		return std::make_shared<ElementAssignmentStatement>(std::move(iep_index), t_operator, std::move(ep_value));
	}
	// In a value arm, the surrounding call, literal, or result list owns the next comma.
	if (!singleValue) {
		ep_expression = parseResults(std::move(ep_expression), t_start.getSpan(), allowWhileTrailer);
	}
	return std::make_shared<ExpressionStatement>(std::move(ep_expression));
}

BlockBody Parser::parseUnbracedBody (const bool allowWhileTrailer, const bool valueArm) {
	const Token               t_start = peek();
	std::vector<StatementPtr> v_statements;
	if (p_bindingParser.startsBinding()) {
		v_statements = p_bindingParser.parseBindings(allowWhileTrailer);
	} else {
		v_statements.push_back(parseStatement(allowWhileTrailer, valueArm));
	}
	const TokenSpan ts_span = TokenSpan::spanThrough(t_start.getSpan(), previousStatementEnd());
	if (!valueArm || !dynamic_cast<const ExpressionStatement *>(v_statements.back().get())) {
		finishStatement(t_start, *v_statements.back());
	}
	return { .span = ts_span, .statements = std::move(v_statements) };
}

TokenSpan Parser::previousStatementEnd () const {
	int i_index = i_currentIndex - 1;
	while (isSemiColon(v_tokens.at(static_cast<std::size_t>(i_index)))) {
		--i_index;
	}
	return v_tokens.at(static_cast<std::size_t>(i_index)).getSpan();
}

ReturnStatement Parser::returnStatement (const bool allowWhileTrailer) {
	if (!b_inFnBody) {
		throw cde::SyntaxErrors::expected(std::format("a {} inside a function body", toString(KReturn)), peek());
	}
	if (bindingBraceHasReturn) {
		*bindingBraceHasReturn = true;
	}
	const Token     t_keyword = advance();
	const Token     t_next    = peek();
	const bool      b_bare    = isSemiColon(t_next) || isBlockEnd(t_next) || atKeyword(KElse) || (allowWhileTrailer && atKeyword(KWhile));
	ExpressionPtr   ep_value  = b_bare ? nullptr : parseResults(nullptr, std::nullopt, allowWhileTrailer);
	const TokenSpan ts_span   = TokenSpan::spanThrough(t_keyword.getSpan(), previousStatementEnd());
	return { t_keyword, std::move(ep_value), ts_span };
}

bool Parser::startsAssignmentList () {
	if (!isReceivingTarget(peek(), /*assignment=*/true)) {
		return false;
	}
	int i_offset = 1;
	while (at(OComma, i_offset)) {
		if (!isReceivingTarget(peek(i_offset + 1), /*assignment=*/true)) {
			return false;
		}
		i_offset += 2;
	}
	return (i_offset > 1 || at(OUnderscore)) && (at(OAssign, i_offset) || at(OFinalAssign, i_offset));
}

bool Parser::isReceivingTarget (const Token &token, const bool assignment) {
	switch (token.getType()) {
	case TokenType::Identifier:
		return true;
	case TokenType::Operator:
		return token.getLexeme() == toString(OUnderscore);
	case TokenType::Keyword:
		return assignment && token.getLexeme() == toString(KSelf);
	default:
		return false;
	}
}

ReceivingTarget Parser::parseReceivingTarget (const bool assignment) {
	if (const Token t_token = peek(); !isReceivingTarget(t_token, assignment)) {
		throw cde::SyntaxErrors::expected(std::format("a binding or assignment target name or '{}'", toString(OUnderscore)), t_token);
	}
	return ReceivingTarget::of(advance());
}

ExpressionPtr Parser::parseResults (ExpressionPtr first, const std::optional<TokenSpan> &start, const bool allowWhileTrailer) {
	const TokenSpan ts_start   = start ? *start : first ? first->getSpan() : peek().getSpan();
	ExpressionPtr   ep_initial = first ? std::move(first) :
	                                     p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
	if (!at(OComma)) {
		return ep_initial;
	}
	std::vector<ExpressionPtr> v_values;
	v_values.push_back(std::move(ep_initial));
	while (at(OComma)) {
		advance();
		v_values.push_back(p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer));
	}
	const TokenSpan ts_span = TokenSpan::spanThrough(ts_start, previousStatementEnd());
	return std::make_shared<ResultListExpression>(std::move(v_values), ts_span);
}

std::vector<TypeReference> Parser::parseTypeList () {
	if (!at(OLess)) {
		return {};
	}
	return p_delimitedParser.angles([this] () -> std::vector<TypeReference> {
		std::vector<TypeReference> v_types;
		v_types.push_back(p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
		while (at(OComma)) {
			advance();
			v_types.push_back(p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
		}
		return v_types;
	});
}

Token Parser::expect (const TokenType type, const std::string &description) {
	if (const Token t_token = peek(); t_token.getType() != type) {
		throw cde::SyntaxErrors::expected(std::move(description), t_token);
	}
	return advance();
}

Token Parser::expect (const Operator op) {
	if (const Token t_token = peek(); t_token.getType() != TokenType::Operator || t_token.getLexeme() != toString(op)) {
		throw cde::SyntaxErrors::expected(std::format("'{}'", toString(op)), t_token);
	}
	return advance();
}

} // namespace cloth
