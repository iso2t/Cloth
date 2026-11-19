// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <gtest/gtest.h>

#include <cde/cloth_diagnostic.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>

namespace cloth::test {

inline CheckedProgram checkSource (const std::string &source, TargetLayout layout = {}) {
	Lexer lexer(source);
	return CheckedProgram(SymbolTable(Parser(lexer).parse()), layout);
}

inline void expectError (const std::string &source, cde::DiagnosticCode code) {
	SCOPED_TRACE(source);
	try {
		checkSource(source);
		FAIL() << "Expected a semantic diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), code) << error.what();
		ASSERT_FALSE(error.get_source_labels().empty());
		EXPECT_NE(error.get_source_labels().front().span.source, nullptr);
	}
}

inline const Expression &bodyExpression (const Symbol &symbol) {
	const CallableSignature &cs_function = *std::get<const CallableSignature *>(symbol.declaration);
	return *std::get<ExpressionBody>(cs_function.executableBody()->value).expression;
}

}
