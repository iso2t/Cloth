// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <gtest/gtest.h>
#include <vector>

#include <cde/cloth_diagnostic.hpp>
#include <cde/diagnostic_code.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>

namespace cde {
namespace {

TEST (DiagnosticsImports, DuplicateSelectionLabelsBothOccurrences) {
	constexpr const char *s_source = "using shapes: Point, Point;";
	cloth::Lexer          l_lexer(s_source);
	cloth::Parser         p_parser(l_lexer);

	try {
		p_parser.parse();
		FAIL() << "Expected a diagnostic for the repeated import selection";
	} catch (const ClothDiagnostic &cd_error) {
		EXPECT_EQ(toString(cd_error.getCode()), "C2010");
		EXPECT_EQ(cd_error.getSeverity(), Severity::Error);
		const std::vector<SourceLabel> &v_labels = cd_error.getSourceLabels();
		ASSERT_EQ(v_labels.size(), 2u);

		const SourceLabel &sl_primary = v_labels[0];
		EXPECT_EQ(sl_primary.text, "here");
		EXPECT_EQ(sl_primary.span.startLine, 1u);
		EXPECT_EQ(sl_primary.span.startCol, 22u);
		EXPECT_EQ(sl_primary.span.endLine, 1u);
		EXPECT_EQ(sl_primary.span.endCol, 27u);

		const SourceLabel &sl_related = v_labels[1];
		EXPECT_EQ(sl_related.text, "First selection here");
		EXPECT_EQ(sl_related.span.startLine, 1u);
		EXPECT_EQ(sl_related.span.startCol, 15u);
		EXPECT_EQ(sl_related.span.endLine, 1u);
		EXPECT_EQ(sl_related.span.endCol, 20u);
		ASSERT_NE(sl_primary.span.source, nullptr);
		EXPECT_EQ(sl_related.span.source, sl_primary.span.source);
		EXPECT_EQ(sl_primary.span.source->getContents(), s_source);
	}
}

}
}
