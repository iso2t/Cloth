// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <semantic/checker.hpp>

namespace cloth::semantic {
namespace {

struct IntrinsicSite {
	std::string_view path;
	std::string_view owner;
	bool             ownership = false;
};

constexpr IntrinsicSite UTF8{ "cloth/text/Utf8.co", {} };
constexpr IntrinsicSite FILE_IO{ "cloth/io/fs/File.co", "File" };
constexpr IntrinsicSite OWNERSHIP{ "cloth/collections/OwningList.co", "OwningList", true };

struct IntrinsicSpec {
	std::string_view     id;
	LibraryIntrinsic     operation;
	const IntrinsicSite &site;
};

constexpr IntrinsicSpec INTRINSICS[] = {
	{ "text.utf8.decode", LibraryIntrinsic::DecodeUtf8, UTF8 },
	{ "io.file.open", LibraryIntrinsic::FileOpen, FILE_IO },
	{ "io.file.read", LibraryIntrinsic::FileRead, FILE_IO },
	{ "collections.ownership.acquire", LibraryIntrinsic::OwnershipAcquire, OWNERSHIP },
	{ "collections.ownership.release", LibraryIntrinsic::OwnershipRelease, OWNERSHIP },
	{ "collections.ownership.delete", LibraryIntrinsic::OwnershipDelete, OWNERSHIP },
	{ "collections.ownership.check_mutation", LibraryIntrinsic::OwnershipCheckMutation, OWNERSHIP },
	{ "collections.ownership.set_cleanup", LibraryIntrinsic::OwnershipSetCleanup, OWNERSHIP },
};

[[noreturn]] void invalidIntrinsic (const IntrinsicBody &body, std::string reason) {
	throw cde::ClothDiagnostic(Code::INVALID_INTRINSIC_BINDING, std::format("Invalid compiler binding '{}': {}", body.get_id(), reason),
	                           { { body.operation.get_span(), "binding declared here" } });
}

const Symbol &fileError (const SymbolTable &unit, const IntrinsicBody &body) {
	constexpr std::string_view        CASES[]    = { "InvalidPath", "NotFound", "PermissionDenied", "OpenFailed", "AlreadyOpen", "NotOpen", "ReadFailed" };
	const SymbolScope                &ss_globals = unit.get_globals();
	const SymbolScope::const_iterator it_enum    = ss_globals.find("FileError");
	if (it_enum != ss_globals.end()) {
		for (const Symbol &sym : it_enum->second) {
			const TypeDeclaration *const *td_type   = std::get_if<const TypeDeclaration *>(&sym.declaration);
			const EnumDeclaration        *ed_errors = td_type ? dynamic_cast<const EnumDeclaration *>(*td_type) : nullptr;
			if (ed_errors && std::ranges::all_of(CASES, [&] (std::string_view name) {
				    return std::ranges::any_of(ed_errors->cases,
				                               [&] (const EnumCaseDeclaration &entry) { return entry.name.get_lexeme() == name; });
			    }))
				return sym;
		}
	}
	invalidIntrinsic(body, "requires the library FileError enum with its registered cases");
}

ResolvedType nullable (ResolvedType type) {
	type.suffixes.emplace_back(to_string(oQuestion));
	return type;
}

ResolvedType bytes () {
	ResolvedType rt_bytes{ std::string(unsignedIntegerSpelling()) };
	rt_bytes.width = 8;
	rt_bytes.suffixes.emplace_back(arraySuffix());
	return rt_bytes;
}

std::string typeList (const ResultTypes &types) {
	std::string s_types;
	for (const ResolvedType &rt_type : types) {
		if (!s_types.empty()) {
			s_types += to_string(oComma);
			s_types += ' ';
		}
		s_types += rt_type.spelling();
	}
	return s_types;
}

[[noreturn]] void invalidMainReturn (const CallableSignature &main) {
	const TokenSpan ts_returns = TokenSpan::span_through(main.returnTypes.front().get_span(), main.returnTypes.back().get_span());
	throw cde::ClothDiagnostic(Code::INVALID_MAIN_RETURN,
	                           std::format("Main must return {}, {}, or a signed {} at a supported width", to_string(kVoid), to_string(kIsize),
	                                       to_string(kInteger)),
	                           { { ts_returns, "invalid entry-point return type" } });
}

}

void Analyzer::registerIntrinsics () {
	// Symbol scopes are ordered by name. Report duplicate bindings in declaration order.
	struct IntrinsicDeclaration {
		const Symbol        *symbol;
		const IntrinsicBody *body;
	};

	std::vector<IntrinsicDeclaration> v_bindings;
	for (const Symbol *sym : v_order) {
		const CallableSignature *cs_signature = callable(*sym);
		const FunctionBody      *fb_body      = cs_signature ? cs_signature->executableBody() : nullptr;
		if (fb_body) {
			if (const IntrinsicBody *ib_body = std::get_if<IntrinsicBody>(&fb_body->value)) v_bindings.push_back({ sym, ib_body });
		}
	}
	std::ranges::sort(v_bindings, {}, [&] (const IntrinsicDeclaration &binding) {
		const TokenSpan ts_operation = binding.body->operation.get_span();
		return std::tuple{ declarations.at(binding.symbol).file, ts_operation.startLine, ts_operation.startCol };
	});
	std::map<LibraryIntrinsic, const IntrinsicBody *> m_registered;
	for (const IntrinsicDeclaration &binding : v_bindings) {
		const Symbol                      &sym          = *binding.symbol;
		const IntrinsicBody               &ib_body      = *binding.body;
		DeclarationInfo                   &di_signature = declarations.at(&sym);
		const LibraryPaths::const_iterator it_paths     = m_libraryPaths.find(di_signature.file);
		if (it_paths == m_libraryPaths.end()) invalidIntrinsic(ib_body, "requires a declaration imported from the selected standard library");
		const IntrinsicSpec *is_spec = std::ranges::find(INTRINSICS, ib_body.get_id(), &IntrinsicSpec::id);
		if (is_spec == std::end(INTRINSICS)) invalidIntrinsic(ib_body, "unknown operation");
		const IntrinsicSite    &is_site    = is_spec->site;
		const ClassDeclaration *cd_owner   = asClass(di_signature.owner);
		const bool              b_function = sym.kind == SymbolKind::Function && !di_signature.owner;
		const bool              b_method   = sym.kind == SymbolKind::Method && cd_owner && !declarations.at(di_signature.owner).owner &&
		                      cd_owner->name.get_lexeme() == is_site.owner;
		if (!(is_site.owner.empty() ? b_function : b_method) || !it_paths->second.contains(is_site.path))
			invalidIntrinsic(ib_body, std::format("requires the registered declaration in {}{}", is_site.path,
			                                      is_site.owner.empty() ? std::string{} : std::format(" on class '{}'", is_site.owner)));
		const CallableSignature &cs_signature = *callable(sym);
		const MethodDeclaration *md_method    = dynamic_cast<const MethodDeclaration *>(&cs_signature);
		if (is_site.ownership && (sym.visibility != Visibility::Private || typeParameters(sym)))
			invalidIntrinsic(ib_body, "requires a private method with no method type parameters");
		if ((cd_owner && (!cd_owner->finalKeyword || (cd_owner->typeParameters && !is_site.ownership))) ||
		    (md_method && (md_method->virtualPrefix || md_method->overridePrefix)) || (!cd_owner && typeParameters(sym)) ||
		    std::ranges::any_of(cs_signature.parameters, &ParameterDeclaration::isOptional))
			invalidIntrinsic(ib_body, std::format("requires a concrete function or ordinary method on a {} class with required parameters",
			                                      to_string(kFinal)));
		const IntrinsicBody *&ib_first = m_registered[is_spec->operation];
		if (ib_first)
			throw cde::ClothDiagnostic(Code::INVALID_INTRINSIC_BINDING, std::format("Compiler binding '{}' is already declared", ib_body.get_id()),
			                           { { ib_body.operation.get_span(), "duplicate binding" },
			                             { ib_first->operation.get_span(), "first declared here" } });
		ib_first               = &ib_body;
		di_signature.intrinsic = is_spec->operation;
		if (is_spec->operation == LibraryIntrinsic::OwnershipSetCleanup) sym_owningList = di_signature.owner;
	}
}

void Analyzer::checkIntrinsicSignature (const Symbol &symbol, LibraryIntrinsic operation) const {
	const DeclarationInfo &di_signature = declarations.at(&symbol);
	const IntrinsicBody   &ib_body      = std::get<IntrinsicBody>(callable(symbol)->executableBody()->value);
	ResultTypes            v_parameters, v_results;
	switch (operation) {
	case LibraryIntrinsic::DecodeUtf8:
		v_parameters = { bytes() };
		v_results    = { nullable(ResolvedType(kString)) };
		break;
	case LibraryIntrinsic::FileOpen:
	case LibraryIntrinsic::FileRead: {
		ResolvedType rt_error = nullable(ownerType(fileError(*program.units[di_signature.file], ib_body)));
		if (operation == LibraryIntrinsic::FileOpen) {
			v_parameters = { ResolvedType(kString) };
			v_results    = { std::move(rt_error) };
		} else {
			v_parameters = { ResolvedType(kUsize) };
			v_results    = { nullable(bytes()), std::move(rt_error) };
		}
		break;
	}
	case LibraryIntrinsic::OwnershipAcquire:
	case LibraryIntrinsic::OwnershipRelease:
	case LibraryIntrinsic::OwnershipDelete:
		v_parameters = { ownerType(objectRoot()) };
		break;
	case LibraryIntrinsic::OwnershipCheckMutation:
		break;
	case LibraryIntrinsic::OwnershipSetCleanup:
		v_parameters = { ResolvedType(kBoolean) };
		break;
	}
	if (di_signature.parameters != v_parameters || di_signature.returns != v_results)
		invalidIntrinsic(ib_body, std::format("expected parameters ({}) and results ({})", typeList(v_parameters), typeList(v_results)));
}

const Symbol *Analyzer::entryPoint () const {
	if (program.kind == ProgramKind::Library) return nullptr;
	const SymbolTable                &st_entry   = *program.units.front();
	const SymbolScope                &ss_globals = st_entry.get_globals();
	const SymbolScope::const_iterator it_main    = ss_globals.find("main");
	Candidates                        v_mains;
	if (it_main != ss_globals.end()) {
		for (const Symbol &sym : it_main->second) {
			if (sym.kind == SymbolKind::Function) v_mains.push_back(&sym);
		}
	}
	if (v_mains.empty()) {
		std::vector<cde::SourceLabel> v_labels;
		if (const std::optional<TokenSpan> &ts_end = st_entry.get_source_unit().end)
			v_labels.push_back({ *ts_end, "no main was declared in this source" });
		throw cde::ClothDiagnostic(Code::MISSING_MAIN, "Executable program requires a main function", std::move(v_labels),
		                           "Declare main in the entry file, or use --kind library to check a library.");
	}
	if (v_mains.size() > 1) {
		std::vector<cde::SourceLabel> v_labels;
		for (const Symbol *sym : v_mains) v_labels.push_back({ sym->name.get_span(), "main declared here" });
		throw cde::ClothDiagnostic(Code::DUPLICATE_MAIN, "Executable program must have exactly one main function", std::move(v_labels));
	}
	const Symbol &sym_main = *v_mains.front();
	if (const TypeParameterList *tp_parameters = typeParameters(sym_main))
		throw cde::ClothDiagnostic(Code::INVALID_MAIN_PARAMETERS, "Main cannot declare type parameters",
		                           { { tp_parameters->get_span(), "entry-point type parameters" } });
	const CallableSignature &cs_main = *callable(sym_main);
	const DeclarationInfo   &di_main = declarations.at(&sym_main);
	if (di_main.returns.size() > 1) invalidMainReturn(cs_main);
	ResolvedType rt_args(kString);
	rt_args.suffixes.push_back(arraySuffix());
	if (!di_main.parameters.empty() && di_main.parameters != ResultTypes{ rt_args } && di_main.parameters != ResultTypes{ nullable(rt_args) })
		throw cde::ClothDiagnostic(Code::INVALID_MAIN_PARAMETERS,
		                           std::format("Main must take no parameters or one {} or {} parameter", rt_args.spelling(),
		                                       nullable(rt_args).spelling()),
		                           { { sym_main.name.get_span(), "invalid entry-point parameter list" } });
	if (!di_main.returns.empty()) {
		const ResolvedType &rt_result = di_main.returns.front();
		if (rt_result != ResolvedType(kIsize) && !(rt_result.integer() && rt_result.name == to_string(kInteger) && rt_result.width))
			invalidMainReturn(cs_main);
	}
	return &sym_main;
}

}
