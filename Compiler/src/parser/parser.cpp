// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
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

#include <ast/source_unit.hpp>
#include <import_errors.hpp>
#include <parser/decl/class_declaration.hpp>
#include <parser/decl/declarations.hpp>
#include <parser/decl/enum_declaration.hpp>
#include <parser/decl/function_declaration.hpp>
#include <parser/decl/import_declaration.hpp>
#include <parser/decl/interface_declaration.hpp>
#include <parser/decl/state_declaration.hpp>
#include <parser/decl/struct_declaration.hpp>
#include <parser/expr/expression.hpp>
#include <parser/expr/expressions.hpp>
#include <parser/expr/provider_expressions.hpp>
#include <parser/expr/struct_expressions.hpp>
#include <parser/parser.hpp>
#include <parser/stmt/do_while_statement.hpp>
#include <parser/stmt/for_statement.hpp>
#include <parser/stmt/loop_control.hpp>
#include <parser/stmt/statement.hpp>
#include <parser/stmt/statements.hpp>
#include <parser/stmt/while_statement.hpp>
#include <parser/type/type_reference.hpp>
#include <syntax_errors.hpp>
#include <token/token.hpp>

namespace cloth {

const std::unordered_set<std::string_view> Parser::assignmentOperators{
	cloth::to_string(Operator::o_ASSIGN),      cloth::to_string(Operator::o_FINAL_ASSIGN), cloth::to_string(Operator::o_PLUS_EQUAL),
	cloth::to_string(Operator::o_MINUS_EQUAL), cloth::to_string(Operator::o_MUL_EQUAL),    cloth::to_string(Operator::o_DIV_EQUAL),
	cloth::to_string(Operator::o_MOD_EQUAL),   cloth::to_string(Operator::o_PLUS_PLUS),    cloth::to_string(Operator::o_MINUS_MINUS),
};

namespace {

const std::unordered_set<std::string_view> UPDATE_OPERATORS{
	cloth::to_string(Operator::o_PLUS_PLUS),
	cloth::to_string(Operator::o_MINUS_MINUS),
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
	return token.get_type() == TokenType::EndOfFile || token.get_lexeme() == cloth::to_string(Operator::o_BRACE_CLOSE);
}

std::string_view declarationDescription (const Declaration &declaration) {
	if (dynamic_cast<const StructDeclaration *>(&declaration)) {
		return "a struct declaration";
	}
	if (dynamic_cast<const ClassDeclaration *>(&declaration)) {
		return "a class declaration";
	}
	if (dynamic_cast<const EnumDeclaration *>(&declaration)) {
		return "an enum declaration";
	}
	if (dynamic_cast<const InterfaceDeclaration *>(&declaration)) {
		return "an interface declaration";
	}
	if (dynamic_cast<const TopLevelStateDeclaration *>(&declaration)) {
		return "a state declaration";
	}
	return {};
}

}

Token Parser::peek (int offset) {
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
	Token t_token = v_tokens.at(static_cast<std::size_t>(i_currentIndex));
	++i_currentIndex;
	return t_token;
}

SourceUnit Parser::parse () {
	return parseDeclarations();
}

SourceUnit Parser::parseDeclarations () {
	std::vector<DeclarationPtr>    v_declarations;
	std::vector<ImportDeclaration> v_imports;
	skipNewLines();

	while (peek().get_type() != TokenType::EndOfFile) {
		if (atKeyword(Keyword::k_USING)) {
			if (!v_declarations.empty()) {
				throw cde::ImportErrors::outsideHeader(peek());
			}
			v_imports.push_back(p_importParser.parse());
			skipNewLines();
			continue;
		}
		rejectMethodOutsideType();
		DeclarationPtr             dp_declaration = startsTypeDeclaration() ? parseTypeDeclaration() :
		                                                                      std::make_shared<FunctionDeclaration>(p_functionParser.parseFunction());
		const FunctionDeclaration *fd_function    = dynamic_cast<const FunctionDeclaration *>(dp_declaration.get());
		const FunctionBody        *fb_body        = fd_function ? &fd_function->body : nullptr;
		const bool                 b_hasEquals    = fb_body && fb_body->get_equals();
		const Token                t_next         = peek();
		if (b_hasEquals && isSemiColon(t_next)) {
			advance();
		} else if (t_next.get_type() != TokenType::NewLine && t_next.get_type() != TokenType::EndOfFile) {
			if (fb_body && !b_hasEquals) {
				throw cde::SyntaxErrors::expected("a newline or end of file after a brace-bodied function", t_next);
			}
			const std::string_view s_description = declarationDescription(*dp_declaration);
			if (!s_description.empty()) {
				throw cde::SyntaxErrors::expected(std::format("a newline or end of file after {}", s_description), t_next);
			}
			throw cde::SyntaxErrors::newline(t_next);
		}
		v_declarations.push_back(std::move(dp_declaration));
		skipNewLines();
	}
	return { std::move(v_declarations), peek().get_span(), std::move(v_imports) };
}

bool Parser::startsTypeDeclaration () {
	int i_offset = atKeyword(Keyword::k_PUBLIC) ? 1 : 0;
	if (atKeyword(Keyword::k_FINAL, i_offset)) {
		++i_offset;
	}
	return atKeyword(Keyword::k_TYPEDEF, i_offset);
}

TypeDeclarationPtr Parser::parseNestedTypeDeclaration () {
	TypeDeclarationPtr tdp_type;
	{
		const ScopedValue<bool> sv_stateBody(b_inStateBody, false);
		tdp_type = std::dynamic_pointer_cast<const TypeDeclaration>(parseTypeDeclaration(/*nested=*/true));
	}
	if (!tdp_type) {
		throw std::bad_cast();
	}
	const Token t_next = peek();
	if (t_next.get_type() != TokenType::NewLine && t_next.get_lexeme() != cloth::to_string(Operator::o_BRACE_CLOSE)) {
		throw cde::SyntaxErrors::expected(std::format("a newline or '{}' after a nested type declaration", cloth::to_string(Operator::o_BRACE_CLOSE)),
		                             t_next);
	}
	skipNewLines();
	return tdp_type;
}

DeclarationPtr Parser::parseTypeDeclaration (bool nested) {
	std::optional<Token>         opt_visibility   = atKeyword(Keyword::k_PUBLIC) ? std::make_optional(advance()) : std::nullopt;
	std::optional<Token>         opt_finalKeyword = atKeyword(Keyword::k_FINAL) ? std::make_optional(advance()) : std::nullopt;
	const Token                  t_keyword        = advance();
	std::optional<TypeReference> opt_payload;
	const Token                  t_kind = p_delimitedParser.angles([this, &opt_visibility, &opt_finalKeyword, &opt_payload, nested] () -> Token {
		const Token t_start = peek();
		if (atKeyword(Keyword::k_CLASS)) {
			return advance();
		}
		if (opt_finalKeyword) {
			throw cde::SyntaxErrors::expected(std::format("'{}' as the type declaration kind after '{}'", cloth::to_string(Keyword::k_CLASS),
			                                         cloth::to_string(Keyword::k_FINAL)),
			                             t_start);
		}
		if (atKeyword(Keyword::k_STRUCT) || atKeyword(Keyword::k_ENUM) || atKeyword(Keyword::k_INTERFACE)) {
			return advance();
		}
		if (!nested && atKeyword(Keyword::k_STATE)) {
			const Token t_state = advance();
			if (opt_visibility) {
				throw cde::SyntaxErrors::expected(std::format("state visibility after the '{}' header", cloth::to_string(Keyword::k_TYPEDEF)),
				                             *opt_visibility);
			}
			opt_payload = p_delimitedParser.angles([this] () -> TypeReference {
				skipNewLines();
				TypeReference tr_payload = p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true);
				skipNewLines();
				return tr_payload;
			});
			return t_state;
		}
		if (nested) {
			throw cde::SyntaxErrors::expected(std::format("'{}', '{}', '{}', or '{}' as the nested type declaration kind",
			                                         cloth::to_string(Keyword::k_STRUCT), cloth::to_string(Keyword::k_CLASS),
			                                         cloth::to_string(Keyword::k_ENUM), cloth::to_string(Keyword::k_INTERFACE)),
			                             t_start);
		}
		throw cde::SyntaxErrors::expected(std::format("'{}', '{}', '{}', '{}', or '{}' as the type declaration kind", cloth::to_string(Keyword::k_STRUCT),
		                                         cloth::to_string(Keyword::k_CLASS), cloth::to_string(Keyword::k_ENUM),
		                                         cloth::to_string(Keyword::k_INTERFACE), cloth::to_string(Keyword::k_STATE)),
		                             t_start);
	});

	const std::string &s_kind = t_kind.get_lexeme();
	if (s_kind == cloth::to_string(Keyword::k_CLASS)) {
		return std::make_shared<ClassDeclaration>(p_classParser.parse(t_keyword, std::move(opt_visibility), std::move(opt_finalKeyword)));
	}
	if (s_kind == cloth::to_string(Keyword::k_ENUM)) {
		return std::make_shared<EnumDeclaration>(p_enumParser.parse(t_keyword, std::move(opt_visibility)));
	}
	if (s_kind == cloth::to_string(Keyword::k_INTERFACE)) {
		return std::make_shared<InterfaceDeclaration>(p_interfaceParser.parse(t_keyword, t_kind, std::move(opt_visibility)));
	}
	if (s_kind == cloth::to_string(Keyword::k_STATE)) {
		return std::make_shared<TopLevelStateDeclaration>(p_classParser.parseTopLevelState(t_keyword, t_kind, std::move(opt_payload.value())));
	}
	return std::make_shared<StructDeclaration>(p_structParser.parse(t_keyword, std::move(opt_visibility)));
}

bool Parser::atKeyword (Keyword keyword, int offset) {
	const Token t_offset = peek(offset);
	return t_offset.get_type() == TokenType::Keyword && t_offset.get_lexeme() == cloth::to_string(keyword);
}

bool Parser::at (Operator op, int offset) {
	return peek(offset).get_lexeme() == cloth::to_string(op);
}

void Parser::rejectMethodOutsideType () {
	const int i_offset = atKeyword(Keyword::k_PUBLIC) ? 1 : 0;
	if (atKeyword(Keyword::k_MT, i_offset)) {
		throw cde::SyntaxErrors::methodOutsideType(peek(i_offset));
	}
}

BlockBody Parser::parseBlock () {
	const Token               t_opening    = peek();
	std::vector<StatementPtr> v_statements = p_delimitedParser.braces([this] () -> std::vector<StatementPtr> { return parseStatements(); });
	const TokenSpan           ts_span      = TokenSpan::span_through(t_opening.get_span(), previous().get_span());
	return { .span = ts_span, .statements = std::move(v_statements) };
}

std::unique_ptr<Expression> Parser::parseBindingBrace (bool allowLiteral, bool allowSingleField,
                                                       std::function<std::unique_ptr<FunctionBlockExpression>(BlockBody)> deferred) {
	const Token               t_opening = peek();
	BindingBraceSyntax        bbs_syntax;
	BindingBraceSyntax       *bbs_boundary = allowLiteral ? &bbs_syntax : nullptr;
	std::vector<StatementPtr> v_statements;
	{
		const ScopedValue<BindingBraceSyntax *> sv_bindingBrace(bbs_bindingBrace, bbs_boundary);
		v_statements = p_functionParser.inBody([this, bbs_boundary] () -> std::vector<StatementPtr> {
			return p_delimitedParser.braces([this, bbs_boundary] () -> std::vector<StatementPtr> { return parseStatements(bbs_boundary); });
		});
	}
	const TokenSpan ts_span = TokenSpan::span_through(t_opening.get_span(), previous().get_span());
	if (bbs_boundary && !bbs_syntax.hasSemicolon) {
		const ExpressionStatement *es_only = v_statements.size() == 1 ? dynamic_cast<const ExpressionStatement *>(v_statements.front().get()) : nullptr;
		const ExpressionPtr        ep_expression = es_only ? es_only->expression : nullptr;
		if (const ResultListExpression *rle_fields = dynamic_cast<const ResultListExpression *>(ep_expression.get())) {
			return std::make_unique<StructLiteral>(rle_fields->values, ts_span);
		}
		if (allowSingleField && !bbs_syntax.hasReturn &&
		    std::all_of(v_statements.begin(), v_statements.end(),
		                [] (const StatementPtr &statement) -> bool { return dynamic_cast<const ExpressionStatement *>(statement.get()) != nullptr; })) {
			if (bbs_syntax.secondStatement) {
				throw cde::SyntaxErrors::expected(std::format("'{}' between struct field values", cloth::to_string(Operator::o_COMMA)),
				                             *bbs_syntax.secondStatement);
			}
			std::vector<ExpressionPtr> v_fields;
			if (ep_expression) {
				v_fields.push_back(ep_expression);
			}
			return std::make_unique<StructLiteral>(std::move(v_fields), ts_span);
		}
	}
	return deferred(BlockBody{ .span = ts_span, .statements = std::move(v_statements) });
}

std::vector<StatementPtr> Parser::parseStatements (BindingBraceSyntax *braceSyntax) {
	std::vector<StatementPtr> v_statements;
	while (true) {
		const Token t_next = peek();
		if (isBlockEnd(t_next)) {
			break;
		}
		if (isStatementSeparator(t_next)) {
			if (braceSyntax && isSemiColon(t_next)) {
				braceSyntax->hasSemicolon = true;
			}
			advance();
			continue;
		}
		if (braceSyntax && !v_statements.empty() && !braceSyntax->secondStatement) {
			braceSyntax->secondStatement = t_next;
		}
		if (atKeyword(Keyword::k_DEF)) {
			std::vector<StatementPtr> v_bindings = p_bindingParser.parseBindings();
			v_statements.insert(v_statements.end(), std::make_move_iterator(v_bindings.begin()), std::make_move_iterator(v_bindings.end()));
		} else {
			v_statements.push_back(parseStatement(/*allowWhileTrailer=*/false, /*multilineResults=*/braceSyntax != nullptr));
		}
		const Token t_separator = peek();
		if (!isStatementSeparator(t_separator) && !isBlockEnd(t_separator)) {
			throw cde::SyntaxErrors::newline(t_separator);
		}
	}
	return v_statements;
}

void Parser::skipNewLines () {
	while (peek().get_type() == TokenType::NewLine) {
		advance();
	}
}

int Parser::afterNewLines (int offset) {
	while (peek(offset).get_type() == TokenType::NewLine) {
		++offset;
	}
	return offset;
}

bool Parser::isSemiColon (const Token &token) {
	return token.get_type() == TokenType::Operator && token.get_lexeme() == cloth::to_string(Operator::o_SEMICOLON);
}

bool Parser::isStatementSeparator (const Token &token) {
	return token.get_type() == TokenType::NewLine || isSemiColon(token);
}

StatementPtr Parser::parseStatement (bool allowWhileTrailer, bool multilineResults) {
	if (atKeyword(Keyword::k_USING)) {
		throw cde::ImportErrors::outsideHeader(peek());
	}
	rejectMethodOutsideType();
	if (atKeyword(Keyword::k_FOR)) {
		return std::make_shared<ForStatement>(p_forLoopParser.parseFor(allowWhileTrailer));
	}
	if (atKeyword(Keyword::k_WHILE)) {
		return std::make_shared<WhileStatement>(p_whileLoopParser.parseWhile(allowWhileTrailer));
	}
	if (atKeyword(Keyword::k_DO)) {
		return std::make_shared<DoWhileStatement>(p_doWhileLoopParser.parseDoWhile());
	}
	if (atKeyword(Keyword::k_BREAK) || atKeyword(Keyword::k_CONTINUE)) {
		return std::make_shared<LoopJumpStatement>(advance());
	}
	if (atKeyword(Keyword::k_RETURN)) {
		return std::make_shared<ReturnStatement>(returnStatement(allowWhileTrailer));
	}
	if (startsAssignmentList()) {
		std::vector<ReceivingTarget> v_targets{ ReceivingTarget::of(advance()) };
		while (at(Operator::o_COMMA)) {
			advance();
			v_targets.push_back(parseReceivingTarget(/*assignment=*/true));
		}
		if (at(Operator::o_FINAL_ASSIGN)) {
			throw cde::SyntaxErrors::reassignment(peek());
		}
		expect(Operator::o_ASSIGN);
		return std::make_shared<MultiAssignmentStatement>(std::move(v_targets), parseResults(nullptr, std::nullopt, allowWhileTrailer));
	}

	const Token t_start     = peek();
	const Token t_following = peek(1);
	if ((t_start.get_type() == TokenType::Identifier || atKeyword(Keyword::k_SELF)) && t_following.get_type() == TokenType::Operator &&
	    assignmentOperators.contains(t_following.get_lexeme())) {
		const Token t_name = advance();
		if (at(Operator::o_FINAL_ASSIGN)) {
			throw cde::SyntaxErrors::reassignment(peek());
		}
		const Token t_operator = advance();
		if (UPDATE_OPERATORS.contains(t_operator.get_lexeme())) {
			return std::make_shared<UpdateStatement>(t_name, t_operator);
		}
		if (t_operator.get_lexeme() == cloth::to_string(Operator::o_ASSIGN)) {
			return std::make_shared<AssignmentStatement>(t_name, parseResults(nullptr, std::nullopt, allowWhileTrailer));
		}
		ExpressionPtr  ep_right = p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
		const Operator o_base   = compound_assignment_base(t_operator.get_lexeme());
		const Token    t_base(std::string(cloth::to_string(o_base)), o_base, t_operator.get_span());
		ExpressionPtr  ep_value = std::make_shared<BinaryExpression>(std::make_shared<NameExpression>(t_name), t_base, std::move(ep_right));
		return std::make_shared<AssignmentStatement>(t_name, std::move(ep_value), t_operator);
	}

	ExpressionPtr      ep_expression = p_expressionParser.parseExpression(0, /*stopAtAssignment=*/true, /*stopAtRange=*/false, allowWhileTrailer);
	const Token        t_operator    = peek();
	const std::string &s_operator    = t_operator.get_lexeme();
	if (assignmentOperators.contains(s_operator)) {
		if (const ProviderAccessExpression *pae_provider = dynamic_cast<const ProviderAccessExpression *>(ep_expression.get())) {
			throw cde::SyntaxErrors::readOnlyProvider(pae_provider->name);
		}
		if (s_operator == cloth::to_string(Operator::o_FINAL_ASSIGN)) {
			throw cde::SyntaxErrors::reassignment(t_operator);
		}
		const bool                             b_fieldTarget = hasFieldTarget(*ep_expression);
		std::shared_ptr<const IndexExpression> iep_index     = std::dynamic_pointer_cast<const IndexExpression>(ep_expression);
		if (!b_fieldTarget && !iep_index) {
			throw cde::SyntaxErrors::expected("an indexed array assignment target", t_operator);
		}
		advance();
		ExpressionPtr ep_value;
		if (s_operator == cloth::to_string(Operator::o_ASSIGN)) {
			ep_value = parseResults(nullptr, std::nullopt, allowWhileTrailer);
		} else if (!UPDATE_OPERATORS.contains(s_operator)) {
			ep_value = p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
		}
		if (b_fieldTarget) {
			return std::make_shared<FieldWriteStatement>(std::move(ep_expression), t_operator, std::move(ep_value));
		}
		return std::make_shared<ElementAssignmentStatement>(std::move(iep_index), t_operator, std::move(ep_value));
	}
	return std::make_shared<ExpressionStatement>(parseResults(std::move(ep_expression), t_start.get_span(), allowWhileTrailer, multilineResults));
}

BlockBody Parser::parseUnbracedBody (bool allowWhileTrailer) {
	const Token               t_start = peek();
	std::vector<StatementPtr> v_statements;
	if (atKeyword(Keyword::k_DEF)) {
		v_statements = p_bindingParser.parseBindings(allowWhileTrailer);
	} else {
		v_statements.push_back(parseStatement(allowWhileTrailer));
	}
	const TokenSpan ts_span = TokenSpan::span_through(t_start.get_span(), previousStatementEnd());
	if (allowWhileTrailer) {
		const Statement           *stmt_only    = v_statements.size() == 1 ? v_statements.front().get() : nullptr;
		const bool                 b_nestedLoop = dynamic_cast<const ForStatement *>(stmt_only) || dynamic_cast<const WhileStatement *>(stmt_only);
		const ExpressionStatement *es_only      = dynamic_cast<const ExpressionStatement *>(stmt_only);
		const bool                 b_bareIf     = es_only && t_start.get_lexeme() == cloth::to_string(Keyword::k_IF) &&
		                      dynamic_cast<const IfExpression *>(es_only->expression.get());
		if (!b_nestedLoop && !b_bareIf) {
			const int i_separator = afterNewLines();
			if (isSemiColon(peek(i_separator)) && atKeyword(Keyword::k_WHILE, afterNewLines(i_separator + 1))) {
				for (int i_consumed = 0; i_consumed <= i_separator; ++i_consumed) {
					advance();
				}
			}
		}
	}
	return { .span = ts_span, .statements = std::move(v_statements) };
}

TokenSpan Parser::previousStatementEnd () {
	int i_index = i_currentIndex - 1;
	while (isStatementSeparator(v_tokens.at(static_cast<std::size_t>(i_index)))) {
		--i_index;
	}
	return v_tokens.at(static_cast<std::size_t>(i_index)).get_span();
}

ReturnStatement Parser::returnStatement (bool allowWhileTrailer) {
	if (!b_inFnBody) {
		throw cde::SyntaxErrors::expected("a return inside a function body", peek());
	}
	if (bbs_bindingBrace) {
		bbs_bindingBrace->hasReturn = true;
	}
	const Token t_keyword = advance();
	const Token t_next    = peek();
	const bool  b_bare    = isStatementSeparator(t_next) || isBlockEnd(t_next) || atKeyword(Keyword::k_ELSE) ||
	                    (allowWhileTrailer && atKeyword(Keyword::k_WHILE));
	ExpressionPtr   ep_value = b_bare ? nullptr : parseResults(nullptr, std::nullopt, allowWhileTrailer);
	const TokenSpan ts_span  = TokenSpan::span_through(t_keyword.get_span(), previousStatementEnd());
	return { t_keyword, std::move(ep_value), ts_span };
}

bool Parser::startsAssignmentList () {
	if (!isReceivingTarget(peek(), /*assignment=*/true)) {
		return false;
	}
	int i_offset = 1;
	while (at(Operator::o_COMMA, i_offset)) {
		if (!isReceivingTarget(peek(i_offset + 1), /*assignment=*/true)) {
			return false;
		}
		i_offset += 2;
	}
	return (i_offset > 1 || at(Operator::o_UNDERSCORE)) && (at(Operator::o_ASSIGN, i_offset) || at(Operator::o_FINAL_ASSIGN, i_offset));
}

bool Parser::isReceivingTarget (const Token &token, bool assignment) {
	switch (token.get_type()) {
	case TokenType::Identifier:
		return true;
	case TokenType::Operator:
		return token.get_lexeme() == cloth::to_string(Operator::o_UNDERSCORE);
	case TokenType::Keyword:
		return assignment && token.get_lexeme() == cloth::to_string(Keyword::k_SELF);
	default:
		return false;
	}
}

ReceivingTarget Parser::parseReceivingTarget (bool assignment) {
	const Token t_token = peek();
	if (!isReceivingTarget(t_token, assignment)) {
		throw cde::SyntaxErrors::expected(std::format("a binding or assignment target name or '{}'", cloth::to_string(Operator::o_UNDERSCORE)), t_token);
	}
	return ReceivingTarget::of(advance());
}

ExpressionPtr Parser::parseResults (ExpressionPtr first, std::optional<TokenSpan> start, bool allowWhileTrailer, bool multiline) {
	const TokenSpan ts_start   = start ? *start : first ? first->get_span() : peek().get_span();
	ExpressionPtr   ep_initial = first ? std::move(first) :
	                                     p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer);
	if (!consumeResultComma(multiline)) {
		return ep_initial;
	}
	std::vector<ExpressionPtr> v_values;
	v_values.push_back(std::move(ep_initial));
	do {
		skipNewLines();
		v_values.push_back(p_expressionParser.parseExpression(0, /*stopAtAssignment=*/false, /*stopAtRange=*/false, allowWhileTrailer));
	} while (consumeResultComma(multiline));
	const TokenSpan ts_span = TokenSpan::span_through(ts_start, previousStatementEnd());
	return std::make_shared<ResultListExpression>(std::move(v_values), ts_span);
}

bool Parser::consumeResultComma (bool multiline) {
	const int i_comma = multiline ? afterNewLines() : 0;
	if (!at(Operator::o_COMMA, i_comma)) {
		return false;
	}
	for (int i_consumed = 0; i_consumed <= i_comma; ++i_consumed) {
		advance();
	}
	return true;
}

std::vector<TypeReference> Parser::parseTypeList () {
	if (!at(Operator::o_LESS)) {
		return {};
	}
	return p_delimitedParser.angles([this] () -> std::vector<TypeReference> {
		skipNewLines();
		std::vector<TypeReference> v_types;
		v_types.push_back(p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
		skipNewLines();
		while (at(Operator::o_COMMA)) {
			advance();
			skipNewLines();
			v_types.push_back(p_typeParser.parse(/*inCast=*/false, /*allowArrays=*/true));
			skipNewLines();
		}
		return v_types;
	});
}

Token Parser::expect (TokenType type, std::string description) {
	const Token t_token = peek();
	if (t_token.get_type() != type) {
		throw cde::SyntaxErrors::expected(std::move(description), t_token);
	}
	return advance();
}

Token Parser::expect (Operator op) {
	const Token t_token = peek();
	if (t_token.get_type() != TokenType::Operator || t_token.get_lexeme() != cloth::to_string(op)) {
		throw cde::SyntaxErrors::expected(std::format("'{}'", cloth::to_string(op)), t_token);
	}
	return advance();
}

} // namespace cloth
