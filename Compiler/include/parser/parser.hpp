// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <ast/source_unit.hpp>
#include <lexer/lexer.hpp>
#include <parser/decl/declarations.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser {
    public:
	Parser (std::vector<Token> tokens) : v_tokens(tokens) {
	}

	Parser (Lexer &lexer) : v_tokens(lexer.scanTokens()) {
	}

    private:
	std::vector<Token> v_tokens;

	int  i_currentIndex = 0;
	bool b_inFnBody     = false;
	bool b_inStateBody  = false;

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
