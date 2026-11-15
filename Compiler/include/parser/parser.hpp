// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include <ast/SourceUnit.hpp>
#include <lexer/lexer.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser {
    public:
	Parser(std::vector<Token> tokens)
		: v_tokens(tokens) {
	}

	Parser(Lexer &lexer)
		: v_tokens(lexer.scanTokens()) {
	}

    private:
	std::vector<Token> v_tokens;

	int i_currentIndex = 0;
	bool b_inFnBody = false;
	bool b_inStateBody = false;

	Token peek(int offset = 0);
	Token previous();
	Token advance();
	SourceUnit parse();
	SourceUnit parseDeclarations();
	bool startsTypeDeclaration();
	// TypeDeclaration parseNestedTypeDeclaration();
	// Declaration parseTypeDeclaration(bool nested = false);
	bool atKeyword(Keyword keyword, int offset = 0);
	void rejectMethodOutsideType();
	// BlockBody parseBlock();
};
}
