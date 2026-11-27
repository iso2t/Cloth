// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/expr/expression.hpp>
#include <ast/source_unit.hpp>
#include <ast/stmt/statement.hpp>
#include <ast/stmt/statements.hpp>
#include <ast/type/type_reference.hpp>
#include <lexer/lexer.hpp>
#include <parser/subs/bindings_parser.hpp>
#include <parser/subs/class_parser.hpp>
#include <parser/subs/delimited_parser.hpp>
#include <parser/subs/enum_parser.hpp>
#include <parser/subs/expression_parser.hpp>
#include <parser/subs/function_parser.hpp>
#include <parser/subs/if_parser.hpp>
#include <parser/subs/import_parser.hpp>
#include <parser/subs/interface_parser.hpp>
#include <parser/subs/loops/do_while_loop_parser.hpp>
#include <parser/subs/loops/for_loop_parser.hpp>
#include <parser/subs/loops/while_loop_parser.hpp>
#include <parser/subs/struct_parser.hpp>
#include <parser/subs/type_parser.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser {
    public:
	explicit Parser (std::vector<Token> tokens);

	explicit Parser (Lexer &lexer) : Parser(lexer.scanTokens()) {
	}

	explicit Parser (Lexer &&lexer) : Parser(std::move(lexer).scanTokens()) {
	}

	SourceUnit parse ();
	SourceUnit parseDeclarations ();

	static const std::unordered_set<std::string_view> ASSIGNMENT_OPERATORS;

    private:
	friend class BindingsParser;
	friend class ClassParser;
	friend class DelimitedParser;
	friend class DoWhileLoopParser;
	friend class EnumParser;
	friend class ExpressionParser;
	friend class ForLoopParser;
	friend class FunctionParser;
	friend class IfParser;
	friend class ImportParser;
	friend class InterfaceParser;
	friend class LoopParser;
	friend class StructParser;
	friend class TypeParser;
	friend class WhileLoopParser;

	std::vector<Token> v_tokens;

	int   i_currentIndex               = 0;
	bool  b_inFnBody                   = false;
	bool  b_inStateBody                = false;
	bool  b_allowInferredOperatorWidth = false;
	bool *bindingBraceHasReturn        = nullptr;

	ExpressionParser  p_expressionParser{ *this };
	TypeParser        p_typeParser{ *this };
	DelimitedParser   p_delimitedParser{ *this };
	ForLoopParser     p_forLoopParser{ *this };
	WhileLoopParser   p_whileLoopParser{ *this };
	DoWhileLoopParser p_doWhileLoopParser{ *this };
	FunctionParser    p_functionParser{ *this };
	IfParser          p_ifParser{ *this };
	BindingsParser    p_bindingParser{ *this };
	StructParser      p_structParser{ *this };
	ClassParser       p_classParser{ *this };
	EnumParser        p_enumParser{ *this };
	InterfaceParser   p_interfaceParser{ *this };
	ImportParser      p_importParser{ *this };

	Token                        peek (int offset = 0);
	Token                        previous ();
	Token                        advance ();
	Token                        requireSemicolon (std::string_view after);
	bool                         consumeEmptyTypeHeader (std::string_view after);
	void                         parseDeclarationItems (bool braced, const std::function<void(const PublicBlockOrigin &)> &parseItem);
	void                         finishStatement (const Token &first, const Statement &last);
	bool                         startsTypeDeclaration ();
	TypeDeclarationPtr           parseNestedTypeDeclaration (const PublicBlockOrigin &publicBlock = {});
	std::shared_ptr<Declaration> parseTypeDeclaration (bool nested = false);
	bool                         atKeyword (Keyword keyword, int offset = 0);
	std::optional<Token>         parseVisibility ();
	bool                         at (Operator op, int offset = 0);
	void                         rejectMethodOutsideType ();
	BlockBody                    parseBlock ();
	std::unique_ptr<Expression>  parseBindingBrace (bool allowLiteral, bool allowSingleField,
	                                                const std::function<std::unique_ptr<FunctionBlockExpression>(BlockBody)> &deferred);
	std::vector<StatementPtr>    parseStatements ();
	static bool                  isSemiColon (const Token &token);
	StatementPtr                 parseStatement (bool allowWhileTrailer = false, bool singleValue = false);
	BlockBody                    parseUnbracedBody (bool allowWhileTrailer = false, bool valueArm = false);
	[[nodiscard]] TokenSpan      previousStatementEnd () const;
	ReturnStatement              returnStatement (bool allowWhileTrailer);
	bool                         startsAssignmentList ();
	static bool                  isReceivingTarget (const Token &token, bool assignment = false);
	ReceivingTarget              parseReceivingTarget (bool assignment = false);
	ExpressionPtr parseResults (ExpressionPtr first = nullptr, const std::optional<TokenSpan> &start = std::nullopt, bool allowWhileTrailer = false);
	std::vector<TypeReference> parseTypeList ();
	Token                      expect (TokenType type, const std::string &description);
	Token                      expect (Operator op);
};
}
