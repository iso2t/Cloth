// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <token/token.hpp>

namespace cloth {

class Parser;

class FunctionParser {
    public:
	explicit FunctionParser (Parser &parser) : p_parser(parser) {
	}

	FunctionDeclaration               parseFunction ();
	std::vector<ParameterDeclaration> parseParameters ();
	ParameterDeclaration              parseParameter ();
	MethodDeclaration          parseMethod (std::optional<Token> visibility = std::nullopt, const std::optional<TokenSpan> &overridePrefix = std::nullopt,
	                                        const std::optional<TokenSpan> &virtualPrefix = std::nullopt, bool allowIntrinsic = false,
	                                        bool allowTypeParameters = false);
	InterfaceMethodDeclaration parseInterfaceMethod ();
	MethodDeclaration          parseOperator (std::optional<Token> visibility = std::nullopt);
	BlockBody                  parseBlockBody ();

	template <typename ParseContents> std::invoke_result_t<ParseContents> inBody (ParseContents &&parseContents) {
		bool             &b_context = functionBodyFlag();
		const RestoreFlag rf_context(b_context, std::exchange(b_context, true));
		return std::forward<ParseContents>(parseContents)();
	}

    private:
	struct MethodHeader {
		Token                             keyword;
		Token                             name;
		std::vector<TypeReference>        returnTypes;
		std::vector<ParameterDeclaration> parameters;
		std::optional<TypeParameterList>  typeParameters;
	};

	class RestoreFlag {
	    public:
		RestoreFlag (bool &flag, bool saved) : b_flag(flag), b_saved(saved) {
		}

		~RestoreFlag () {
			b_flag = b_saved;
		}

		RestoreFlag (const RestoreFlag &)           = delete;
		RestoreFlag &operator=(const RestoreFlag &) = delete;

	    private:
		bool &b_flag;
		bool  b_saved;
	};

	Parser &p_parser;

	MethodHeader            methodHeader (bool allowTypeParameters);
	InterfaceMethodFallback parseInterfaceFallback ();
	Token                   parseOperatorName () const;
	FunctionBody            parseBody (std::string_view kind, bool allowStatement = false, bool allowIntrinsic = false);
	FunctionBody            parseVoidBody (const Token &equals) const;
	bool                   &functionBodyFlag () const;
	bool                    at (Operator op, int offset = 0) const;
};

}
