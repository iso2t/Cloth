// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <filesystem>
#include <set>
#include <string>

#include "checked_program_test.hpp"

namespace cloth::test {
namespace {

using Code   = cde::DiagnosticCode;
namespace fs = std::filesystem;

CheckedProgram checkEntry (std::string source, ProgramKind kind = ProgramKind::Executable, TargetLayout layout = {}) {
	Lexer l_lexer{ SourceFile("entry.co", std::move(source)) };
	return CheckedProgram(SymbolTable(Parser(l_lexer).parse()), layout, {}, kind);
}

std::vector<cde::SourceLabel> entryError (const std::string &source, Code expected) {
	SCOPED_TRACE(source);
	try {
		checkEntry(source);
		ADD_FAILURE() << "Expected an entry diagnostic";
		return {};
	} catch (const cde::ClothDiagnostic &error) {
		EXPECT_EQ(error.get_code(), expected) << error.what();
		for (const cde::SourceLabel &sl_label : error.get_source_labels()) {
			EXPECT_NE(sl_label.span.source, nullptr);
			if (sl_label.span.source) {
				EXPECT_EQ(sl_label.span.source->get_path(), "entry.co");
			}
		}
		return error.get_source_labels();
	}
}

TEST (SemanticProgram, EntryBelongsToTheExecutableAndBodiesAreAlwaysChecked) {
	EXPECT_EQ(checkSource("fn helper() {}").kind, ProgramKind::Library);
	EXPECT_EQ(checkEntry("fn main(int arg) {}\nfn main() {}", ProgramKind::Library).entryPoint, nullptr);
	const CheckedProgram cp_program = checkEntry("typedef<class> App { mt main() {} }\nfn main() {}");
	EXPECT_EQ(cp_program.kind, ProgramKind::Executable);
	EXPECT_EQ(cp_program.entryPoint, &cp_program.units.front()->get_globals().at("main").front());
	entryError("typedef<class> App { mt main() {} }", Code::MISSING_MAIN);
	entryError("fn main() {}\nfn<int> main() = 0", Code::DUPLICATE_FUNCTION);
	entryError("fn<int> broken() = true", Code::RETURN_TYPE);
	expectError("fn<int> broken() = true", Code::RETURN_TYPE);
}

TEST (SemanticProgram, EntryDiagnosticsRetainTheEofAndDeclarationSpans) {
	const std::vector<cde::SourceLabel> v_empty = entryError("", Code::MISSING_MAIN);
	ASSERT_EQ(v_empty.size(), 1u);
	EXPECT_EQ(v_empty.front().span.to_string(), "1:1-1:1");
	const std::vector<cde::SourceLabel> v_eof = entryError("fn helper() {}\r\n", Code::MISSING_MAIN);
	ASSERT_EQ(v_eof.size(), 1u);
	EXPECT_EQ(v_eof.front().span.to_string(), "2:1-2:1");
	const std::vector<cde::SourceLabel> v_mains = entryError("fn main() {}\nfn main(string[] args) {}\nfn main(int n) {}", Code::DUPLICATE_MAIN);
	ASSERT_EQ(v_mains.size(), 3u);
	for (std::size_t st_index = 0; st_index < v_mains.size(); ++st_index) {
		EXPECT_EQ(v_mains[st_index].span.startLine, st_index + 1);
		EXPECT_EQ(v_mains[st_index].span.startCol, 4u);
		EXPECT_EQ(v_mains[st_index].span.endCol, 8u);
	}
	const std::vector<cde::SourceLabel> v_generic = entryError("fn main<T>() {}", Code::INVALID_MAIN_PARAMETERS);
	ASSERT_EQ(v_generic.size(), 1u);
	EXPECT_EQ(v_generic.front().span.to_string(), "1:8-1:11");
	const std::vector<cde::SourceLabel> v_params = entryError("fn main(int n) {}", Code::INVALID_MAIN_PARAMETERS);
	ASSERT_EQ(v_params.size(), 1u);
	EXPECT_EQ(v_params.front().span.to_string(), "1:4-1:8");
	const std::vector<cde::SourceLabel> v_returns = entryError("fn<int, bool> main(int arg) = 1, true", Code::INVALID_MAIN_RETURN);
	ASSERT_EQ(v_returns.size(), 1u);
	EXPECT_EQ(v_returns.front().span.to_string(), "1:4-1:13");
}

TEST (SemanticProgram, StartupArgumentsAndSignedExitTypes) {
	for (const char *s_params : { "", "string[] args", "final string[] values", "string[]? args", "final opt<string[]?> args ?: null" }) {
		const CheckedProgram cp_program = checkEntry(std::string("pub fn main(") + s_params + ") {}");
		ASSERT_NE(cp_program.entryPoint, nullptr);
	}
	for (const char *s_params : { "string args", "string[][] args", "string?[] args", "string?[]? args", "string[2] args", "string[] args, int count" })
		entryError(std::string("fn main(") + s_params + ") {}", Code::INVALID_MAIN_PARAMETERS);
	for (const unsigned u_bits : { 32u, 64u }) {
		for (const char *s_type : { "int", "int<8>", "int<16>", "int<32>", "int<64>", "isize" }) {
			const CheckedProgram cp_program =
			        checkEntry(std::string("fn<") + s_type + "> main() = 0", ProgramKind::Executable, TargetLayout{ u_bits });
			ASSERT_NE(cp_program.entryPoint, nullptr);
		}
	}
	EXPECT_NE(checkEntry("fn<void> main() {}").entryPoint, nullptr);
	for (const char *s_type : { "usize", "unsigned int<8>", "int?", "bool", "float", "int[]" }) {
		const std::string s_typeName(s_type);
		entryError("fn<" + s_typeName + "> main() = main()", Code::INVALID_MAIN_RETURN);
	}
}

const ExpressionInfo &bodyCall (const CheckedProgram &program, const char *name) {
	const Symbol &sym_caller = program.units.front()->get_globals().at(name).front();
	return program.expressions.at(&bodyExpression(sym_caller));
}

TEST (SemanticProgram, IntrinsicsFollowSelectedOverloadsAndSpecializedMethods) {
	const fs::path             p_inputs = fs::path(__FILE__).parent_path().parent_path() / "Inputs" / "boundaries";
	Lexer                      l_lexer{ SourceFile(p_inputs / "client.co") };
	const CheckedProgram       cp_program(SymbolTable(Parser(l_lexer).parse()), {}, p_inputs / "library", ProgramKind::Executable);
	std::set<LibraryIntrinsic> s_bound;
	for (const decltype(cp_program.declarations)::value_type &entry : cp_program.declarations) {
		if (entry.second.intrinsic) {
			EXPECT_TRUE(s_bound.insert(*entry.second.intrinsic).second) << entry.first->name.get_lexeme();
		}
	}
	EXPECT_EQ(s_bound, (std::set{ LibraryIntrinsic::DecodeUtf8, LibraryIntrinsic::FileOpen, LibraryIntrinsic::FileRead, LibraryIntrinsic::OwnershipAcquire,
	                              LibraryIntrinsic::OwnershipRelease, LibraryIntrinsic::OwnershipDelete, LibraryIntrinsic::OwnershipCheckMutation,
	                              LibraryIntrinsic::OwnershipSetCleanup }));
	EXPECT_EQ(cp_program.declarations.at(bodyCall(cp_program, "decoded").symbol).intrinsic, LibraryIntrinsic::DecodeUtf8);
	EXPECT_FALSE(cp_program.declarations.at(bodyCall(cp_program, "echo").symbol).intrinsic);
	EXPECT_FALSE(cp_program.declarations.at(bodyCall(cp_program, "ordinary").symbol).intrinsic);
	const DeclarationInfo &di_open = cp_program.declarations.at(bodyCall(cp_program, "open").symbol);
	EXPECT_EQ(di_open.intrinsic, LibraryIntrinsic::FileOpen);
	const ExpressionInfo &ei_read = bodyCall(cp_program, "read");
	EXPECT_EQ(cp_program.declarations.at(ei_read.symbol).intrinsic, LibraryIntrinsic::FileRead);
	ASSERT_EQ(ei_read.types.size(), 2u);
	EXPECT_EQ(ei_read.types[1].declaration, di_open.returns.front().declaration);
	const ExpressionInfo &ei_claim = bodyCall(cp_program, "claim");
	ASSERT_NE(ei_claim.instance, nullptr);
	const ExpressionInfo &ei_acquire = ei_claim.instance->body.expressions.at(&bodyExpression(*ei_claim.symbol));
	ASSERT_NE(ei_acquire.instance, nullptr);
	EXPECT_EQ(ei_acquire.instance->signature.intrinsic, LibraryIntrinsic::OwnershipAcquire);
	EXPECT_EQ(ei_acquire.symbol->visibility, Visibility::Private);
	ASSERT_EQ(ei_acquire.instance->signature.parameters.size(), 1u);
	ASSERT_NE(ei_acquire.instance->signature.parameters.front().declaration, nullptr);
	EXPECT_EQ(ei_acquire.instance->signature.parameters.front().declaration->name.get_lexeme(), "Object");
}

TEST (SemanticProgram, OwnershipObjectIsTheBuiltInReferenceRoot) {
	const CheckedProgram cp_program = checkSource("typedef<class> Item\ntypedef<interface> View {}\ntypedef<class> Rooted : Object()\n"
	                                              "fn<Object> item(Item v) = v\nfn<Object> view(View v) = v");
	const SymbolScope   &ss_globals = cp_program.units.front()->get_globals();
	const ResolvedType  &rt_root    = cp_program.declarations.at(&ss_globals.at("item").front()).returns.front();
	EXPECT_EQ(cp_program.declarations.at(&ss_globals.at("Item").front()).parents, ResultTypes{ rt_root });
	EXPECT_EQ(cp_program.declarations.at(&ss_globals.at("Rooted").front()).parents, ResultTypes{ rt_root });
	EXPECT_TRUE(cp_program.parentConstructors.empty());
	expectError("typedef<class> Object", Code::INVALID_CLASS_MEMBER);
	expectError("fn<Object> make() = Object::new", Code::INVALID_CLASS_LIFECYCLE);
	expectError("typedef<class> Child : Object(1)", Code::INVALID_CLASS_MEMBER);
	expectError("fn<Object> box(int n) = n", Code::RETURN_TYPE);
	expectError("typedef<struct> Item {}\nfn<Object> box(Item v) = v", Code::RETURN_TYPE);
}

}
}
