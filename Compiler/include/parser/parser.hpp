// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <ast/source_unit.hpp>
#include <lexer/lexer.hpp>
#include <parser/decl/declarations.hpp>
#include <parser/expr/expression.hpp>
#include <parser/stmt/statement.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser {
    public:
	Parser (std::vector<Token> tokens) : v_tokens(tokens) {
	}

	Parser (Lexer &lexer) : v_tokens(lexer.scanTokens()) {
	}

	struct BindingBraceSyntax {
		bool                 hasReturn       = false;
		bool                 hasSemicolon    = false;
		std::optional<Token> secondStatement = std::nullopt;
	};

    private:
	std::vector<Token> v_tokens;

	int                               i_currentIndex               = 0;
	bool                              b_inFnBody                   = false;
	bool                              b_inStateBody                = false;
	bool                              b_allowInferredOperatorWidth = false;
	std::optional<BindingBraceSyntax> bindingBrace                 = std::nullopt;

	ExpressionParser  p_expressionParser{ this };
	TypeParser        p_typeParser{ this };
	DelimitedParser   p_delimitedParser{ this };
	ForLoopParser     p_forLoopParser{ this };
	WhileLoopParser   p_whileLoopParser{ this };
	DoWhileLoopParser p_doWhileLoopParser{ this };
	FunctionParser    p_functionParser{ this };
	IfParser          p_ifParser{ this };
	BindingParser     p_bindingParser{ this };
	StructParser      p_structParser{ this };
	ClassParser       p_classParser{ this };
	EnumParser        p_enumParser{ this };
	InterfaceParser   p_interfaceParser{ this };
	ImportParser      p_importParser{ this };

	Token                            peek (int offset = 0);
	Token                            previous ();
	Token                            advance ();
	SourceUnit                       parse ();
	SourceUnit                       parseDeclarations ();
	std::unique_ptr<bool>            startsTypeDeclaration ();
	std::unique_ptr<TypeDeclaration> parseNestedTypeDeclaration ();
	Declaration                      parseTypeDeclaration (bool nested = false);
	bool                             atKeyword (Keyword keyword, int offset = 0);
	void                             rejectMethodOutsideType ();
	BlockBody                        parseBlock ();
	std::unique_ptr<Expression>      parseBindingBrace (bool allowLiteral, bool allowSingleField,
	                                                    std::function<std::unique_ptr<FunctionBlockExpression>(BlockBody)> deferred);
	std::vector<Statement>           parseStatements (std::optional<BindingBraceSyntax> braceSyntax = std::nullopt);
	void                             skipNewLines ();
	bool                             isSemiColon (Token token);
	bool                             isStatementSeparator (Token token);
	Statement                        parseStatement (bool allowWhileTrailer = false, bool multilineResults = false);
	BlockBody                        parseUnbracedBody (bool allowWhileTrailer = false);
	TokenSpan                        previousStatementEnd ();
	ReturnStatement                  returnStatement (bool allowWhileTrailer);
	bool                             startsAssignmentList ();
	bool                             isReceivingTarget (Token token, bool assignment = false);
	ReceivingTarget                  parseReceivingTarget (bool assignment = false);
	Expression                       parseResults (std::optional<Expression> first, TokenSpan span, bool allowWhileTrailer, bool multiline);
	std::vector<TypeReference>       parseTypeList ();
	Token                            expect (TokenType type, std::string description);
	Token                            expect (Operator op);
};
}
