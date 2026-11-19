// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <filesystem>
#include <string>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {

using Code = cde::DiagnosticCode;

const std::filesystem::path P_SEMANTIC = std::filesystem::path(__FILE__).parent_path().parent_path();
const std::filesystem::path P_STDLIB   = (P_SEMANTIC / "../../../Stdlib").lexically_normal();

const std::string S_DECLARATIONS = R"(using cloth.collections.OwningList: OwningList
fn opaque(OwningList<Object> list) { list.removeAt(0) }
fn<int> releaseOne(OwningList<Object> list) { list.removeAt(0); return 0 }
typedef<class> ReleasingConstructor(int tag) {
    pub ReleasingConstructor(OwningList<Object> list) : (releaseOne(list)) {}
    pub ReleasingConstructor(OwningList<Object> list, int tag) : (tag) { opaque(list) }
}
typedef<class> DelegatedDefault(OwningList<Object> list, opt<int> tag ?: releaseOne(list)) {
    pub DelegatedDefault(OwningList<Object> list, bool useDefault) : (list) {}
}
typedef<struct> Releaser {
    OwningList<Object> list;
    pub operator ==(Releaser other) { list.removeAt(0); return true }
}
typedef<class> Item {
mt test(OwningList<Object> list, OwningList<Object> other, Object item, bool flag, usize index, Releaser[] probes, int[] numbers) {
)";

CheckedProgram checkOwnership (const std::string &body, const std::filesystem::path &library = P_STDLIB) {
	Lexer l_lexer{ SourceFile("ownership.co", S_DECLARATIONS + body + "\n}\n}") };
	return CheckedProgram(SymbolTable(Parser(l_lexer).parse()), {}, library, ProgramKind::Library);
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

TEST (SemanticOwnership, StdlibIndexReadUsesThePrivateBoundsCheck) {
	Lexer                 l_lexer{ SourceFile(P_SEMANTIC / "owning-list.co") };
	const CheckedProgram  cp_program(SymbolTable(Parser(l_lexer).parse()), {}, P_STDLIB, ProgramKind::Executable);
	const Symbol         &sym_read = cp_program.units.front()->get_globals().at("readAt").front();
	const ExpressionInfo &ei_read  = cp_program.expressions.at(&bodyExpression(sym_read));
	ASSERT_TRUE(ei_read.indexed);
	const IndexedAccess &ia_index = *ei_read.indexed;
	ASSERT_NE(ia_index.validator, nullptr);
	ASSERT_NE(ia_index.validatorInstance, nullptr);
	EXPECT_EQ(ia_index.validator->visibility, Visibility::Private);

	const BinaryExpression &be_bounds = dynamic_cast<const BinaryExpression &>(bodyExpression(*ia_index.validator));
	EXPECT_EQ(be_bounds.op.get_operator(), oLess);
	const BodyFacts         &bf_validator = ia_index.validatorInstance->body;
	const CallableSignature &cs_validator = *std::get<const CallableSignature *>(ia_index.validator->declaration);
	EXPECT_EQ(bf_validator.expressions.at(be_bounds.left.get()).binding, &cs_validator.parameters.front().name);
	const ExpressionInfo &ei_limit = bf_validator.expressions.at(be_bounds.right.get());
	ASSERT_TRUE(ei_limit.provider);
	EXPECT_EQ(ei_limit.provider->kind, ProviderKind::ArraySize);
}

TEST (SemanticOwnership, DiagnosesProvenOwnersAndCycles) {
	expectOwnershipError("def alias = item; list.append(item); alias::delete", "Only the owning list", true);
	expectOwnershipError("list.append(item); list.append(item)", "already belongs", true);
	expectOwnershipError("list.append(item); def value = Item::new; other.append(item)", "already belongs", true);
	expectOwnershipError("list.append(item); def next = OwningList<Object>::new; next.append(item)", "already belongs", true);
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
	       "list.append(item); opaque(list); item::delete", "list.append(item); def released = ReleasingConstructor::new(list); item::delete",
	       "list.append(item); def released = ReleasingConstructor::new(list, 0); item::delete",
	       "list.append(item); def released = DelegatedDefault::new(list, true); item::delete",
	       "list.append(item); def equal = probes == probes; item::delete", "for (def held <- list) { list.removeAt(0); held::delete }",
	       "list.append(item); if (false) item::delete" }) {
		SCOPED_TRACE(s_body);
		EXPECT_NO_THROW(checkOwnership(s_body));
	}
	expectOwnershipError("def a = Item::new; def b = Item::new; list.clear(); list.append(a); list.append(b); list.removeAt(0); b::delete",
	                     "Only the owning list", true);
	expectOwnershipError(
	        "def a = OwningList<Object>::new; def b = OwningList<Object>::new; b.append(Item::new); a.append(item); b.removeAt(index); item::delete",
	        "Only the owning list", true);
	expectOwnershipError("list.append(item); def equal = numbers == numbers; item::delete", "Only the owning list", true);
	EXPECT_NO_THROW(checkSource(
	        "typedef<class> OwningList { pub mt append(Object item) {} }\nfn f(OwningList list, Object item) { list.append(item); list.append(item); item::delete }"));
}

TEST (SemanticOwnership, CleanupDeletesDirectAndNestedEntriesInOrder) {
	for (const char *s_cleanup : { "outer.clear()", "outer::delete" }) {
		const std::string s_body = std::string("def inner = OwningList<Object>::new; def outer = OwningList<Object>::new; ") +
		                           "outer.append(inner); inner.append(item); " + s_cleanup + "; item::delete";
		try {
			checkOwnership(s_body);
			FAIL() << s_body;
		} catch (const cde::ClothDiagnostic &error) {
			EXPECT_EQ(error.get_code(), Code::DELETED_CLASS_ACCESS) << error.what();
		}
	}
	// The first entry is opaque and its destructor can release the nested child.
	EXPECT_NO_THROW(checkOwnership("def inner = OwningList<Object>::new; def outer = OwningList<Object>::new; "
	                               "def child = Item::new; outer.append(item); outer.append(inner); inner.append(child); outer.clear(); child::delete"));
}

TEST (SemanticOwnership, TreatsAnUnrelatedAppendOverloadAsAnOrdinaryCall) {
	const std::filesystem::path p_library = P_SEMANTIC / "Inputs/collection/standard";
	EXPECT_NO_THROW(checkOwnership("list.append(item); list.append(0 -> usize); item::delete", p_library));
}
}
}
