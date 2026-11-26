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
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/type/type_parameters.hpp>
#include <diagnostics.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/program_kind.hpp>
#include <semantic/symbol_table.hpp>
#include <token/token.hpp>

namespace cloth::semantic {
namespace {

struct IntrinsicSite {
	std::string_view path;
	std::string_view owner;
	bool             ownership = false;
	std::optional<PrimitiveFamily> family    = {};
};

constexpr IntrinsicSite UTF8{ .path = "cloth/text/Utf8.co", .owner = {} };
constexpr IntrinsicSite FILE_IO{ .path = "cloth/io/fs/File.co", .owner = "File" };
constexpr IntrinsicSite OWNERSHIP{ .path = "cloth/collections/OwningList.co", .owner = "OwningList", .ownership = true };
constexpr IntrinsicSite INTEGER{ .path = NUMERIC_MODULE, .owner = "Integer", .family = PrimitiveFamily::Integer };
constexpr IntrinsicSite FLOAT{ .path = NUMERIC_MODULE, .owner = "Float", .family = PrimitiveFamily::Float };
constexpr IntrinsicSite NUMERIC{ .path = NUMERIC_MODULE, .owner = {} };

struct IntrinsicSpec {
	std::string_view     id;
	LibraryIntrinsic     operation;
	const IntrinsicSite &site;
};

constexpr IntrinsicSpec INTRINSICS[] = {
	{ .id = "integer.leading_zeros", .operation = LibraryIntrinsic::IntegerLeadingZeros, .site = INTEGER },
	{ .id = "integer.trailing_zeros", .operation = LibraryIntrinsic::IntegerTrailingZeros, .site = INTEGER },
	{ .id = "integer.to_string_radix", .operation = LibraryIntrinsic::IntegerToStringRadix, .site = INTEGER },
	{ .id = "float.abs", .operation = LibraryIntrinsic::FloatAbs, .site = FLOAT },
	{ .id = "float.format", .operation = LibraryIntrinsic::FloatFormat, .site = FLOAT },
	{ .id = "numeric.error", .operation = LibraryIntrinsic::NumericError, .site = NUMERIC },
	{ .id = "text.utf8.decode", .operation = LibraryIntrinsic::DecodeUtf8, .site = UTF8 },
	{ .id = "io.file.open", .operation = LibraryIntrinsic::FileOpen, .site = FILE_IO },
	{ .id = "io.file.read", .operation = LibraryIntrinsic::FileRead, .site = FILE_IO },
	{ .id = "collections.ownership.acquire", .operation = LibraryIntrinsic::OwnershipAcquire, .site = OWNERSHIP },
	{ .id = "collections.ownership.release", .operation = LibraryIntrinsic::OwnershipRelease, .site = OWNERSHIP },
	{ .id = "collections.ownership.delete", .operation = LibraryIntrinsic::OwnershipDelete, .site = OWNERSHIP },
	{ .id = "collections.ownership.check_mutation", .operation = LibraryIntrinsic::OwnershipCheckMutation, .site = OWNERSHIP },
	{ .id = "collections.ownership.set_cleanup", .operation = LibraryIntrinsic::OwnershipSetCleanup, .site = OWNERSHIP },
};

[[noreturn]] void invalidIntrinsic (const IntrinsicBinding &body, std::string reason) {
	throw cde::ClothDiagnostic(Code::InvalidIntrinsicBinding, std::format("Invalid compiler binding '{}': {}", body.getId(), reason),
	                           { { .span = body.operation.getSpan(), .text = "binding declared here" } });
}

const Symbol &fileError (const SymbolTable &unit, const IntrinsicBody &body) {
	const SymbolScope         &ss_globals = unit.getGlobals();
	const auto                 it_enum    = ss_globals.find("FileError");
	if (it_enum != ss_globals.end()) {
		for (const Symbol &sym : it_enum->second) {
			const TypeDeclaration *const *td_type   = std::get_if<const TypeDeclaration *>(&sym.declaration);
			const EnumDeclaration        *ed_errors = td_type ? dynamic_cast<const EnumDeclaration *>(*td_type) : nullptr;
			if (ed_errors && std::ranges::all_of(FILE_ERROR_CASES, [&] (std::string_view name) {
				    return std::ranges::any_of(ed_errors->cases,
				                               [&] (const EnumCaseDeclaration &entry) { return entry.name.getNameKey() == name; });
			    }))
				return sym;
		}
	}
	invalidIntrinsic(body, "requires the library FileError enum with its registered cases");
}

ResolvedType nullable (ResolvedType type) {
	type.suffixes.emplace_back(toString(OQuestion));
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
			s_types += toString(OComma);
			s_types += ' ';
		}
		s_types += rt_type.spelling();
	}
	return s_types;
}

[[noreturn]] void invalidMainReturn (const CallableSignature &main) {
	const TokenSpan ts_returns = TokenSpan::spanThrough(main.returnTypes.front().getSpan(), main.returnTypes.back().getSpan());
	throw cde::ClothDiagnostic(Code::InvalidMainReturn,
	                           std::format("Main must return {}, {}, or a signed {} at a supported width", toString(KVoid), toString(KIsize),
	                                       toString(KInteger)),
	                           { { .span = ts_returns, .text = "invalid entry-point return type" } });
}

}

void Analyzer::registerIntrinsics () {
	// Symbol scopes are ordered by name. Report duplicate bindings in declaration order.
	struct IntrinsicDeclaration {
		const Symbol        *symbol;
		const IntrinsicBinding *body;
	};

	std::vector<IntrinsicDeclaration> v_bindings;
	for (const Symbol *sym : v_order) {
		if (const auto *type = asIntrinsicType(sym)) v_bindings.push_back({ .symbol = sym, .body = &type->binding });
		const CallableSignature *cs_signature = callable(*sym);
		const FunctionBody      *fb_body      = cs_signature ? cs_signature->executableBody() : nullptr;
		if (fb_body) {
			if (const IntrinsicBody *ib_body = std::get_if<IntrinsicBody>(&fb_body->value))
				v_bindings.push_back({ .symbol = sym, .body = ib_body });
		}
	}
	std::ranges::sort(v_bindings, {}, [&] (const IntrinsicDeclaration &binding) {
		const TokenSpan ts_operation = binding.body->operation.getSpan();
		return std::tuple{ declarations.at(binding.symbol).file, ts_operation.startLine, ts_operation.startCol };
	});
	std::map<LibraryIntrinsic, const IntrinsicBinding *> m_registered;
	for (const IntrinsicDeclaration &binding : v_bindings) {
		const Symbol        &sym          = *binding.symbol;
		const IntrinsicBinding &ib_body      = *binding.body;
		DeclarationInfo     &di_signature = declarations.at(&sym);
		const auto           it_paths     = m_libraryPaths.find(di_signature.file);
		if (it_paths == m_libraryPaths.end()) invalidIntrinsic(ib_body, "requires a declaration imported from the selected standard library");
		if (const auto *type = asIntrinsicType(&sym)) {
			if (!type->self.primitiveSelf) invalidIntrinsic(ib_body, "unknown primitive family");
			if (!it_paths->second.contains(NUMERIC_MODULE) || di_signature.owner || sym.visibility != Visibility::Public)
				invalidIntrinsic(ib_body, "requires a public top-level declaration in " + std::string(NUMERIC_MODULE));
			if (!m_primitiveFamilies.emplace(*type->self.primitiveSelf, &sym).second)
				invalidIntrinsic(ib_body,
				                 std::format("the {} family already has a declaration", primitiveFamilyName(*type->self.primitiveSelf)));
			continue;
		}
		const IntrinsicSpec *is_spec = std::ranges::find(INTRINSICS, ib_body.getId(), &IntrinsicSpec::id);
		if (is_spec == std::end(INTRINSICS)) invalidIntrinsic(ib_body, "unknown operation");
		const IntrinsicSite    &is_site    = is_spec->site;
		const ClassDeclaration *cd_owner   = asClass(di_signature.owner);
		const bool              b_function = sym.kind == SymbolKind::Function && !di_signature.owner;
		const bool              b_method   = sym.kind == SymbolKind::Method && cd_owner && !declarations.at(di_signature.owner).owner &&
		                      cd_owner->name.getNameKey() == is_site.owner;
		if (is_site.family) {
			const auto *owner     = asIntrinsicType(di_signature.owner);
			const bool  formatter = is_spec->operation == LibraryIntrinsic::FloatFormat;
			const bool  member    = formatter ? sym.kind == SymbolKind::Operator && sym.name.getNameKey() == provider::S_STRING :
			                                    sym.kind == SymbolKind::Method;
			if (!member || !owner || owner->self.primitiveSelf != is_site.family || !it_paths->second.contains(is_site.path))
				invalidIntrinsic(ib_body, std::format("requires {} on the {} primitive family in {}", formatter ? "operator ::str" : "a method",
				                                      is_site.owner, is_site.path));
		} else if (!(is_site.owner.empty() ? b_function : b_method) || !it_paths->second.contains(is_site.path))
			invalidIntrinsic(ib_body, std::format("requires the registered declaration in {}{}", is_site.path,
			                                      is_site.owner.empty() ? std::string{} : std::format(" on class '{}'", is_site.owner)));
		const CallableSignature &cs_signature = *callable(sym);
		const auto              *md_method    = dynamic_cast<const MethodDeclaration *>(&cs_signature);
		if (is_site.ownership && (sym.visibility != Visibility::Private || typeParameters(sym)))
			invalidIntrinsic(ib_body, "requires a private method with no method type parameters");
		const bool optional = std::ranges::any_of(cs_signature.parameters, &ParameterDeclaration::isOptional);
		if (is_site.family && (optional || typeParameters(sym)))
			invalidIntrinsic(ib_body, "requires a primitive member with required parameters and no method type parameters");
		if ((cd_owner && (!cd_owner->finalKeyword || (cd_owner->typeParameters && !is_site.ownership))) ||
		    (md_method && (md_method->virtualPrefix || md_method->overridePrefix)) || (!cd_owner && typeParameters(sym)) || optional)
			invalidIntrinsic(ib_body, std::format("requires a concrete function or ordinary method on a {} class with required parameters",
			                                      toString(KFinal)));
		const IntrinsicBinding *&ib_first = m_registered[is_spec->operation];
		if (ib_first)
			throw cde::ClothDiagnostic(Code::InvalidIntrinsicBinding, std::format("Compiler binding '{}' is already declared", ib_body.getId()),
			                           { { .span = ib_body.operation.getSpan(), .text = "duplicate binding" },
			                             { .span = ib_first->operation.getSpan(), .text = "first declared here" } });
		ib_first               = &ib_body;
		di_signature.intrinsic = is_spec->operation;
		if (is_spec->operation == LibraryIntrinsic::OwnershipSetCleanup) sym_owningList = di_signature.owner;
	}
}

void Analyzer::checkIntrinsicSignature (const Symbol &symbol, LibraryIntrinsic operation) const {
	const DeclarationInfo &di_signature = declarations.at(&symbol);
	const auto            &ib_body      = std::get<IntrinsicBody>(callable(symbol)->executableBody()->value);
	ResultTypes            v_parameters, v_results;
	switch (operation) {
	case LibraryIntrinsic::IntegerLeadingZeros:
	case LibraryIntrinsic::IntegerTrailingZeros:
		v_results = { ResolvedType(KUsize) };
		break;
	case LibraryIntrinsic::IntegerToStringRadix:
		v_parameters = { ResolvedType(KInteger) };
		[[fallthrough]];
	case LibraryIntrinsic::FloatFormat:
		v_results = { ResolvedType(KString) };
		break;
	case LibraryIntrinsic::FloatAbs:
		v_results = { ownerType(*di_signature.owner) };
		break;
	case LibraryIntrinsic::NumericError:
		v_parameters = { ResolvedType(KString) };
		break;
	case LibraryIntrinsic::DecodeUtf8:
		v_parameters = { bytes() };
		v_results    = { nullable(ResolvedType(KString)) };
		break;
	case LibraryIntrinsic::FileOpen:
	case LibraryIntrinsic::FileRead: {
		ResolvedType rt_error = nullable(ownerType(fileError(*program.units[di_signature.file], ib_body)));
		if (operation == LibraryIntrinsic::FileOpen) {
			v_parameters = { ResolvedType(KString) };
			v_results    = { std::move(rt_error) };
		} else {
			v_parameters = { ResolvedType(KUsize) };
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
		v_parameters = { ResolvedType(KBoolean) };
		break;
	}
	if (di_signature.parameters != v_parameters || di_signature.returns != v_results)
		invalidIntrinsic(ib_body, std::format("expected parameters ({}) and results ({})", typeList(v_parameters), typeList(v_results)));
}

const Symbol *Analyzer::entryPoint () const {
	if (program.kind == ProgramKind::Library) return nullptr;
	const SymbolTable &st_entry   = *program.units.front();
	const SymbolScope &ss_globals = st_entry.getGlobals();
	const auto         it_main    = ss_globals.find("main");
	Candidates         v_mains;
	if (it_main != ss_globals.end()) {
		for (const Symbol &sym : it_main->second) {
			if (sym.kind == SymbolKind::Function) v_mains.push_back(&sym);
		}
	}
	if (v_mains.empty()) {
		std::vector<cde::SourceLabel> v_labels;
		if (const std::optional<TokenSpan> &ts_end = st_entry.getSourceUnit().end)
			v_labels.push_back({ .span = *ts_end, .text = "no main was declared in this source" });
		throw cde::ClothDiagnostic(Code::MissingMain, "Executable program requires a main function", std::move(v_labels),
		                           "Declare main in the entry file, or use --kind library to check a library.");
	}
	if (v_mains.size() > 1) {
		std::vector<cde::SourceLabel> v_labels;
		for (const Symbol *sym : v_mains) v_labels.push_back({ .span = sym->name.getSpan(), .text = "main declared here" });
		throw cde::ClothDiagnostic(Code::DuplicateMain, "Executable program must have exactly one main function", std::move(v_labels));
	}
	const Symbol &sym_main = *v_mains.front();
	if (const TypeParameterList *tp_parameters = typeParameters(sym_main))
		throw cde::ClothDiagnostic(Code::InvalidMainParameters, "Main cannot declare type parameters",
		                           { { .span = tp_parameters->getSpan(), .text = "entry-point type parameters" } });
	const CallableSignature &cs_main = *callable(sym_main);
	const DeclarationInfo   &di_main = declarations.at(&sym_main);
	if (di_main.returns.size() > 1) invalidMainReturn(cs_main);
	ResolvedType rt_args(KString);
	rt_args.suffixes.push_back(arraySuffix());
	if (!di_main.parameters.empty() && di_main.parameters != ResultTypes{ rt_args } && di_main.parameters != ResultTypes{ nullable(rt_args) })
		throw cde::ClothDiagnostic(Code::InvalidMainParameters,
		                           std::format("Main must take no parameters or one {} or {} parameter", rt_args.spelling(),
		                                       nullable(rt_args).spelling()),
		                           { { .span = sym_main.name.getSpan(), .text = "invalid entry-point parameter list" } });
	if (!di_main.returns.empty()) {
		const ResolvedType &rt_result = di_main.returns.front();
		if (rt_result != ResolvedType(KIsize) && !(rt_result.integer() && rt_result.name == toString(KInteger) && rt_result.width))
			invalidMainReturn(cs_main);
	}
	return &sym_main;
}

}
