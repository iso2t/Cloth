// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <filesystem>
#include <string>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {

using Code = cde::DiagnosticCode;

const std::string S_DECLARATIONS = R"(using cloth.collections.OwningList: OwningList
using cloth.collections.Snapshot: Snapshot
fn opaque(OwningList<Object> list) { list.removeAt(0) }
typedef<struct> Releaser {
    OwningList<Object> list;
    operator ==(Releaser other) { list.removeAt(0); return true }
}
typedef<class> Item {
mt test(OwningList<Object> list, OwningList<Object> other, Object item, bool flag, usize index, Snapshot<Object> snapshot, Releaser[] probes, int[] numbers) {
)";

CheckedProgram checkOwnership (const std::string &body) {
	const std::filesystem::path p_library = std::filesystem::path(__FILE__).parent_path().parent_path() / "Inputs/collection/standard";
	Lexer                       l_lexer{ SourceFile("ownership.co", S_DECLARATIONS + body + "\n}\n}") };
	return CheckedProgram(SymbolTable(Parser(l_lexer).parse()), {}, p_library, ProgramKind::Library);
}

void expectOwnershipError (const std::string &body, const std::string &message, bool acquisition) {
	SCOPED_TRACE(body);
	try {
		checkOwnership(body);
		FAIL() << "Expected an ownership diagnostic";
	} catch (const cde::ClothDiagnostic &error) {
		ASSERT_EQ(error.get_code(), Code::INVALID_COLLECTION_OWNERSHIP) << error.what();
		EXPECT_NE(std::string(error.what()).find(message), std::string::npos);
		ASSERT_EQ(error.get_source_labels().size(), acquisition ? 2u : 1u);
		EXPECT_EQ(error.get_source_labels().front().text, "invalid ownership operation");
		if (acquisition) {
			EXPECT_EQ(error.get_source_labels().back().text, "ownership acquired here");
		}
	}
}

TEST (SemanticOwnership, DiagnosesProvenOwnersAndCycles) {
	expectOwnershipError("def alias = item; list.append(item); alias::delete", "Only the owning list", true);
	expectOwnershipError("list.append(item); list.append(item)", "already belongs", true);
	expectOwnershipError("list.append(item); def value = Item::new; other.append(item)", "already belongs", true);
	expectOwnershipError("list.append(list)", "ownership cycle", false);
	expectOwnershipError("list.append(other); other.append(list)", "ownership cycle", true);
	expectOwnershipError("if (flag) list.append(item) else other.append(item); item::delete", "Only the owning list", false);
	expectOwnershipError("list.clear(); list.append(item); list.get(0)::delete", "Only the owning list", true);
	expectOwnershipError("list.clear(); list.append(item); list[0]::delete", "Only the owning list", true);
	expectOwnershipError("list.clear(); list.append(item); for (def held <- list) held::delete", "Only the owning list", true);
	expectOwnershipError("list.clear(); list.append(item); def held = list.removeAt(0); other.append(held); item::delete", "Only the owning list", true);
	for (const char *s_body : { "list.get(index)::delete", "list[index]::delete", "for (def held <- list) held::delete" })
		expectOwnershipError(s_body, "Only the owning list", false);
}

TEST (SemanticOwnership, ReleasesOnlyProvenEntriesAndForgetsOpaqueEffects) {
	for (const char *s_body :
	     { "list.append(item)", "def alias = item; list.append(item); list.removeAt(index); alias::delete",
	       "list.append(item); if (flag) list.removeAt(0); item::delete", "list.append(item); other.removeAt(0); item::delete",
	       "list.clear(); list.append(item); list.append(Item::new); def held = list[index]; list.removeAt(0); held::delete",
	       "def a = Item::new; def b = Item::new; list.clear(); list.append(a); list.append(b); def held = if (flag) a else b; list.removeAt(0); held::delete",
	       "list.append(item); opaque(list); item::delete", "list.append(item); list.append(0 -> usize); item::delete",
	       "list.append(item); def equal = probes == probes; item::delete", "for (def held <- list) { list.removeAt(0); held::delete }",
	       "list.append(item); if (false) item::delete" }) {
		SCOPED_TRACE(s_body);
		EXPECT_NO_THROW(checkOwnership(s_body));
	}
	expectOwnershipError("def a = Item::new; def b = Item::new; list.clear(); list.append(a); list.append(b); list.removeAt(0); b::delete",
	                     "Only the owning list", true);
	expectOwnershipError(
	        "def a = OwningList<Object>::new(snapshot); def b = OwningList<Object>::new(snapshot); a.append(item); b.removeAt(index); item::delete",
	        "Only the owning list", true);
	expectOwnershipError("list.append(item); def equal = numbers == numbers; item::delete", "Only the owning list", true);
	EXPECT_NO_THROW(checkSource(
	        "typedef<class> OwningList { pub mt append(Object item) {} }\nfn f(OwningList list, Object item) { list.append(item); list.append(item); item::delete }"));
}

TEST (SemanticOwnership, CleanupDeletesDirectAndNestedEntriesInOrder) {
	for (const char *s_cleanup : { "outer.clear()", "outer::delete" }) {
		const std::string s_body = std::string("def inner = OwningList<Object>::new(snapshot); def outer = OwningList<Object>::new(snapshot); ") +
		                           "outer.clear(); outer.append(inner); inner.append(item); " + s_cleanup + "; item::delete";
		try {
			checkOwnership(s_body);
			FAIL() << s_body;
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.get_code(), Code::DELETED_CLASS_ACCESS) << error.what();
		}
	}
	// The first entry is opaque and its destructor can release the nested child.
	EXPECT_NO_THROW(checkOwnership(
	        "def inner = OwningList<Object>::new(snapshot); def outer = OwningList<Object>::new(snapshot); "
	        "outer.clear(); def child = Item::new; outer.append(item); outer.append(inner); inner.append(child); outer.clear(); child::delete"));
}

}
}
