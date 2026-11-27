// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Frontend.rst

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <ast/decl/class_declaration.hpp>
#include <ast/decl/declarations.hpp>
#include <ast/decl/enum_declaration.hpp>
#include <ast/decl/function_declaration.hpp>
#include <ast/decl/import_declaration.hpp>
#include <ast/decl/interface_declaration.hpp>
#include <ast/decl/state_declaration.hpp>
#include <ast/decl/struct_declaration.hpp>
#include <ast/expr/expression.hpp>
#include <ast/expr/expressions.hpp>
#include <ast/type/type_parameters.hpp>
#include <ast/type/type_reference.hpp>
#include <diagnostics.hpp>
#include <file/source_file.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/checker.hpp>
#include <semantic/native_module.hpp>
#include <semantic/program_kind.hpp>
#include <semantic/symbol_table.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>
#include <util/unicode_identifier.hpp>
#include <util/utf8.hpp>

namespace cloth {

ResolvedType::ResolvedType (const Keyword keyword) : ResolvedType(std::string(toString(keyword))) {
}

ResolvedType::ResolvedType (std::string name) : name(std::move(name)) {
	if (this->name == toString(KInteger) || this->name == semantic::unsignedIntegerSpelling() || this->name == toString(KFloat)) {
		width = 32;
	}
}

std::strong_ordering ResolvedType::operator<=>(const ResolvedType &other) const {
	if (const std::strong_ordering so_decl = std::compare_three_way{}(declaration, other.declaration); so_decl != 0) return so_decl;
	if (const std::strong_ordering so_param = std::compare_three_way{}(parameter, other.parameter); so_param != 0) return so_param;
	if (const std::strong_ordering so_head =
	            std::tie(name, width, suffixes, projectedSuffixes) <=> std::tie(other.name, other.width, other.suffixes, other.projectedSuffixes);
	    so_head != 0)
		return so_head;
	return application == other.application ? std::strong_ordering::equal : arguments() <=> other.arguments();
}

bool ResolvedType::operator==(const ResolvedType &other) const {
	return *this <=> other == 0;
}

const ResultTypes &ResolvedType::arguments () const {
	static constexpr ResultTypes V_EMPTY;
	return application ? application->values : V_EMPTY;
}

bool ResolvedType::dependent () const {
	return parameter || (application && application->dependent);
}

unsigned ResolvedType::argumentDepth () const {
	return application ? application->depth : 0;
}

void ResolvedType::visitApplications (const std::function<void(const ResolvedType &)> &visit) const {
	std::set<ResolvedType>                          s_seen;
	const std::function<void(const ResolvedType &)> walk = [&] (const ResolvedType &type) {
		if (!type.application || !s_seen.insert(type).second) return;
		for (const ResolvedType &rt_argument : type.arguments()) walk(rt_argument);
		visit(type);
	};
	walk(*this);
}

bool ResolvedType::nullable () const {
	return !suffixes.empty() && suffixes.back() == toString(OQuestion);
}

bool ResolvedType::array () const {
	return !suffixes.empty() && suffixes.back().starts_with(toString(OBracketOpen));
}

std::optional<std::uint64_t> ResolvedType::arrayExtent () const {
	if (!array() || suffixes.back() == semantic::arraySuffix()) return {};
	return std::stoull(suffixes.back().substr(toString(OBracketOpen).size()));
}

std::optional<PrimitiveFamily> ResolvedType::primitiveFamily () const {
	if (!suffixes.empty()) return {};
	if (parameter) return parameter->primitiveSelf;
	if (declaration) return {};
	if (name == toString(KString)) return PrimitiveFamily::String;
	if (name == toString(KCharacter)) return PrimitiveFamily::Char;
	if (name == toString(KBoolean)) return PrimitiveFamily::Bool;
	if (name == toString(KFloat)) return PrimitiveFamily::Float;
	if (name == toString(KInteger) || name == semantic::unsignedIntegerSpelling() || name == toString(KUsize) || name == toString(KIsize))
		return PrimitiveFamily::Integer;
	return {};
}

bool ResolvedType::integer () const {
	return primitiveFamily() == PrimitiveFamily::Integer;
}

bool ResolvedType::floating () const {
	return primitiveFamily() == PrimitiveFamily::Float;
}

bool ResolvedType::enumeration () const {
	return semantic::enumeration(*this) != nullptr;
}

bool ResolvedType::classReference () const {
	return suffixes.empty() && semantic::asClass(declaration);
}

bool ResolvedType::objectReference () const {
	return suffixes.empty() && (semantic::asClass(declaration) || semantic::asInterface(declaration));
}

bool ResolvedType::payloadFreeEnum () const {
	const EnumDeclaration *ed_enum = semantic::enumeration(*this);
	return ed_enum && ed_enum->fields.empty();
}

bool ResolvedType::unsignedInteger () const {
	return suffixes.empty() && (name == semantic::unsignedIntegerSpelling() || name == toString(KUsize));
}

bool ResolvedType::convertsIntegerToBool (const ResolvedType &source) const {
	if (name != toString(KBoolean)) return false;
	if (suffixes.empty()) return source.integer();
	return suffixes.size() == 1 && nullable() && (source.nullable() ? source.element().integer() : source.integer());
}

ResolvedType ResolvedType::element () const {
	ResolvedType rt_element = *this;
	rt_element.suffixes.pop_back();
	return rt_element;
}

std::string ResolvedType::spelling () const {
	constexpr std::size_t      ST_MAX_BYTES = 4096;
	constexpr std::string_view S_ELLIPSIS   = "…";
	std::string                s_result;
	bool                       b_truncated = false;
	const std::function        append      = [&] (const std::string_view part) {
                if (b_truncated) return;
                const std::size_t st_left = ST_MAX_BYTES - s_result.size();
                s_result.append(part.substr(0, st_left));
                if (part.size() <= st_left) return;
                std::size_t st_end = ST_MAX_BYTES - S_ELLIPSIS.size();
                while (st_end && (static_cast<unsigned char>(s_result[st_end]) & 0xc0) == 0x80) --st_end;
                s_result.resize(st_end);
                s_result += S_ELLIPSIS;
                b_truncated = true;
	};
	const std::function<void(const ResolvedType &)> write = [&] (const ResolvedType &type) {
		if (type.parameter)
			append(type.parameter->name.getLexeme());
		else if (type.declaration)
			append(type.declaration->name.getLexeme());
		else
			append(type.name);
		if (type.width && type.width != 32) append(std::format("{}{}{}", toString(OLess), type.width, toString(OGreater)));
		if (!type.arguments().empty()) {
			append(toString(OLess));
			for (const ResolvedType &rt_argument : type.arguments()) {
				if (b_truncated) return;
				if (&rt_argument != &type.arguments().front()) {
					append(toString(OComma));
					append(" ");
				}
				write(rt_argument);
			}
			append(toString(OGreater));
		}
		auto extent = type.suffixes.rbegin();
		for (const std::string &suffix : type.suffixes) {
			if (suffix == toString(OQuestion)) {
				append(suffix);
			} else {
				while (*extent == toString(OQuestion)) ++extent;
				append(*extent++);
			}
		}
	};
	write(*this);
	return s_result;
}

std::optional<ResolvedType> ExpressionInfo::representedAt (const std::size_t slot) const {
	if (slot < representedResults.size()) return representedResults[slot];
	return slot == 0 ? representedType : std::nullopt;
}

std::optional<bool> ExpressionInfo::integerTruth () const {
	const IntegerConstant *ic_value = std::get_if<IntegerConstant>(&numericConstant);
	if (!ic_value) return std::nullopt;
	return std::ranges::any_of(ic_value->magnitude, [] (const std::uint32_t word) { return word != 0; });
}

ResultKind ExpressionInfo::resultKind () const {
	if (!fallsThrough) return ResultKind::Never;
	if (types.empty()) return ResultKind::Empty;
	return types.size() == 1 ? ResultKind::Single : ResultKind::Multiple;
}

ExpressionInfo ExpressionInfo::never () {
	return { .types = {}, .fallsThrough = false };
}

bool BodyFacts::isDirectReceiver (const Expression &expression) const {
	const Expression *e_root = &expression;
	while (const auto *ce_cast = dynamic_cast<const CastExpression *>(e_root)) {
		// A conversion operator can return a different instance. A state transition has no result.
		if (const auto it_cast = expressions.find(ce_cast);
		    it_cast == expressions.end() || it_cast->second.argumentContract || it_cast->second.types.empty())
			return false;
		e_root = ce_cast->operand.get();
	}
	const auto *ne_name = dynamic_cast<const NameExpression *>(e_root);
	return ne_name && ne_name->name.getLexeme() == toString(KSelf);
}

const MethodRef *TypeDispatch::methodTarget (const CallDispatch &call) const {
	if (call.kind == DispatchKind::Interface) {
		const ResolvedType &view = call.receiver.classReference() ? call.contract.owner : call.receiver;
		for (const InterfaceTarget &target : interfaceTargets)
			if (target.view == view && target.contract == call.contract) return &target.implementation;
	} else if (call.kind == DispatchKind::Virtual) {
		for (const MethodSlot &slot : methods)
			if (slot.contract == call.contract && slot.implementation) return &slot.implementation.value();
	}
	return nullptr;
}

const StateMethod *TypeDispatch::stateTarget (const MethodRef &contract) const {
	for (const StateMethod &method : stateMethods)
		if (method.contract == contract) return &method;
	return nullptr;
}

const GenericInstance *CheckedProgram::typeInstance (const ResolvedType &type) const {
	if (!type.declaration || type.arguments().empty()) return nullptr;
	return &instances.at({ .declaration = type.declaration, .receiver = std::nullopt, .arguments = type.arguments() });
}

CheckedProgram::CheckedProgram (SymbolTable entry, const TargetLayout layout, std::filesystem::path standardLibraryRoot, const ProgramKind kind,
                                const NativeModules &modules) :
        layout(layout),
        standardLibraryRoot(std::move(standardLibraryRoot)),
        kind(kind),
        recordExternInterfaces(modules.recordExternInterfaces) {
	if (layout.sizeBits != 32 && layout.sizeBits != 64) throw std::invalid_argument("A Cloth target requires a 32- or 64-bit size type");
	if (layout.scalar64Alignment32 != 4 && layout.scalar64Alignment32 != 8) throw std::invalid_argument("Invalid 32-bit target scalar alignment");
	units.emplace_back(std::make_unique<SymbolTable>(std::move(entry)));
	semantic::Analyzer analyzer(*this, modules);
	analyzer.checkBodies();
	entryPoint = analyzer.entryPoint();
}

std::vector<const Expression *> CheckedProgram::enumFieldValues (const Symbol &field) const {
	const Symbol &sym_owner = *declarations.at(&field).owner;
	const auto   &ed_enum   = static_cast<const EnumDeclaration &>(*std::get<const TypeDeclaration *>(sym_owner.declaration));
	// Field symbols point into this declaration's field vector. Checked case values use the same order.
	const auto                      st_column = static_cast<std::size_t>(std::get<const EnumFieldDeclaration *>(field.declaration) - ed_enum.fields.data());
	std::vector<const Expression *> v_values(ed_enum.cases.size());
	for (const auto &val : sym_owner.members | std::views::values) {
		for (const Symbol &member : val) {
			if (member.kind == SymbolKind::EnumCase) v_values.at(member.enumCaseOrdinal) = enumPayloads.at(&member).at(st_column);
		}
	}
	return v_values;
}
}

namespace cloth::semantic {

std::string_view unsignedIntegerSpelling () {
	static const std::string S_UNSIGNED_INTEGER = toStringmulti({ KUnsigned, KInteger });
	return S_UNSIGNED_INTEGER;
}

std::string arraySuffix (const std::string_view extent) {
	std::string s_suffix(toString(OBracketOpen));
	s_suffix += extent;
	s_suffix += toString(OBracketClose);
	return s_suffix;
}

void fail (const Code code, const TokenSpan &span, std::string message, const Token *related) {
	std::vector<cde::SourceLabel> v_labels{ { .span = span, .text = message } };
	if (related) {
		v_labels.push_back({ .span = related->getSpan(), .text = "declared here" });
	}
	throw cde::ClothDiagnostic(code, std::move(message), std::move(v_labels));
}

const CallableSignature *callable (const Symbol &symbol) {
	const CallableSignature *const *cs_value = std::get_if<const CallableSignature *>(&symbol.declaration);
	return cs_value ? *cs_value : nullptr;
}

const TypeParameterList *typeParameters (const Symbol &symbol) {
	const std::optional<TypeParameterList> *opt_parameters = nullptr;
	if (const CallableSignature *cs_callable = callable(symbol)) {
		if (const auto *fn = dynamic_cast<const FunctionDeclaration *>(cs_callable)) {
			opt_parameters = &fn->typeParameters;
		} else if (const auto *mt = dynamic_cast<const MethodDeclaration *>(cs_callable)) {
			opt_parameters = &mt->typeParameters;
		} else if (const auto *mt = dynamic_cast<const InterfaceMethodDeclaration *>(cs_callable)) {
			opt_parameters = &mt->typeParameters;
		} else if (const auto *mt = dynamic_cast<const StateMethodContractDeclaration *>(cs_callable)) {
			opt_parameters = &mt->typeParameters;
		}
	} else if (symbol.kind == SymbolKind::Type) {
		const auto *const td_type = std::get<const TypeDeclaration *>(symbol.declaration);
		if (const auto *cd_class = dynamic_cast<const ClassDeclaration *>(td_type)) {
			opt_parameters = &cd_class->typeParameters;
		} else if (const auto *sd_struct = dynamic_cast<const StructDeclaration *>(td_type)) {
			opt_parameters = &sd_struct->typeParameters;
		} else if (const auto *id_interface = dynamic_cast<const InterfaceDeclaration *>(td_type)) {
			opt_parameters = &id_interface->typeParameters;
		}
	}
	return opt_parameters && *opt_parameters ? &**opt_parameters : nullptr;
}

Analyzer::Analyzer (CheckedProgram &program, const NativeModules &modules) : program(program), declarations(program.declarations) {
	declarations.emplace(&objectRoot(), DeclarationInfo{ .file = 0, .owner = nullptr });
	loadImports(modules);
	b_hasReferenceDeclarations = std::ranges::any_of(v_order, [] (const Symbol *symbol) { return asClass(symbol) || asInterface(symbol); });
	registerIntrinsics();
	for (const Symbol *sym : v_order) checkParameterNames(*sym);
	for (const Symbol *sym : v_order) {
		resolveDeclaration(*sym);
		if (const std::optional<LibraryIntrinsic> op = declarations.at(sym).intrinsic) checkIntrinsicSignature(*sym, *op);
	}
	checkDeclarations();
}

void Analyzer::registerScope (const SymbolScope &scope, const std::size_t file, const Symbol *owner) {
	for (const SymbolScope::value_type &entry : scope) {
		for (const Symbol &sym : entry.second) {
			if (sym.kind == SymbolKind::Type && sym.name.getNameKey() == objectRoot().name.getNameKey())
				fail(Code::InvalidClassMember, sym.name.getSpan(), std::format("{} is the built-in class root", sym.name.getLexeme()));
			declarations.emplace(&sym, DeclarationInfo{ .file = file, .owner = owner });
			if (!owner && entry.first == "Number" && sym.visibility == Visibility::Public && asInterface(&sym) && !typeParameters(sym) &&
			    !program.standardLibraryRoot.empty()) {
				const SourceFile *sf_source = program.units[file]->getSourceUnit().getSource();
				std::error_code   ec_file;
				if (sf_source && std::filesystem::equivalent(sf_source->getPath(), program.standardLibraryRoot / NUMERIC_MODULE, ec_file))
					program.numberInterface = &sym;
			}
			if (const auto *intrinsic = asIntrinsicType(&sym)) m_genericOwners.emplace(&intrinsic->self, &sym);
			if (sym.kind == SymbolKind::Constructor && dynamic_cast<const PrimaryConstructorDeclaration *>(callable(sym))) {
				declarations.at(owner).primaryConstructor = &sym;
			}
			v_order.push_back(&sym);
			if (const TypeParameterList *parameters = typeParameters(sym)) {
				for (const TypeParameterDeclaration &parameter : parameters->parameters) {
					m_genericOwners.emplace(&parameter, &sym);
					for (const TypeParameterBound &bound : parameter.bounds) {
						if (const ConstructorBound *ctor = std::get_if<ConstructorBound>(&bound.value)) {
							const Symbol &requirement = program.constructorRequirements.emplace_back(
							        SymbolKind::Constructor, Visibility::Public, ctor->declaration.keyword,
							        static_cast<const CallableSignature *>(&ctor->declaration), SymbolScope{});
							declarations.emplace(&requirement, DeclarationInfo{ .file = file, .owner = &sym });
							m_guarantees[&parameter].push_back(&requirement);
							v_order.push_back(&requirement);
						}
					}
				}
			}
			if (!owner) {
				v_names[file][entry.first].symbols.push_back(&sym);
			}
			registerScope(sym.members, file, &sym);
		}
	}
}

namespace {
namespace fs = std::filesystem;

using ModuleDirectory   = std::map<std::string, std::vector<fs::path>>;
using ModuleDirectories = std::map<fs::path, ModuleDirectory>;

std::optional<std::string> moduleFileKey (const fs::path &path) {
	// A directory can also contain native filenames that cannot appear in Cloth source.
	try {
		std::string s_filename = pathUtf8(path.filename());
		if (utf8ValidPrefixLength(s_filename) != s_filename.size()) return std::nullopt;
		const std::string s_extension = lowercaseExtension(path);
		if (!s_filename.ends_with(s_extension)) {
			// Preserve native extension lookup on case-insensitive filesystems.
			fs::path p_lower = path;
			p_lower.replace_extension(s_extension);
			if (std::error_code ec; fs::equivalent(path, p_lower, ec))
				s_filename.replace(s_filename.size() - s_extension.size(), s_extension.size(), s_extension);
		}
		return normalizeIdentifier(s_filename);
	} catch (const fs::filesystem_error &error) {
		if (error.code() != std::errc::illegal_byte_sequence) throw;
	}
	return std::nullopt;
}

// Index each directory once using the same name keys as the symbol table.
// Retain the physical paths since filesystems can store either Unicode spelling.
fs::path moduleEntry (ModuleDirectories &directories, const fs::path &parent, const std::string &key, bool directory, const TokenSpan &use) {
	if (parent.empty() || !fs::is_directory(parent)) return {};
	const std::pair<ModuleDirectories::iterator, bool> it_parent = directories.try_emplace(parent);
	ModuleDirectory                                   &md_names  = it_parent.first->second;
	if (it_parent.second) {
		std::error_code              ec;
		const fs::directory_iterator di_entries(parent, ec);
		if (ec == std::errc::permission_denied) {
			// Search-only directories still permit opening a known filename.
			directories.erase(it_parent.first);
			const fs::path p_exact = parent / std::u8string(key.begin(), key.end());
			return (directory ? fs::is_directory(p_exact) : fs::is_regular_file(p_exact)) ? p_exact : fs::path{};
		}
		if (ec) throw fs::filesystem_error("Cannot read module directory", parent, ec);
		for (const fs::directory_entry &de_entry : di_entries) {
			if (const std::optional<std::string> opt_key = moduleFileKey(de_entry.path())) md_names[*opt_key].push_back(de_entry.path());
		}
	}
	fs::path   p_match;
	const auto it_entries = md_names.find(key);
	if (it_entries == md_names.end()) return {};
	for (const fs::path &p_entry : it_entries->second) {
		if (directory ? !fs::is_directory(p_entry) : !fs::is_regular_file(p_entry)) continue;
		if (!p_match.empty()) fail(Code::AmbiguousImportSource, use, "Multiple filesystem entries have the same normalized module name");
		p_match = p_entry;
	}
	return p_match;
}

}

void Analyzer::loadImports (const NativeModules &modules) {
	namespace fs = std::filesystem;
	using Edge   = std::pair<const ImportDeclaration *, std::size_t>;

	struct ImportRoot {
		const fs::path &path;
		bool            standardLibrary;
	};

	std::vector<std::vector<Edge>>  v_edges;
	std::optional<std::size_t>      preludeFile;
	std::map<fs::path, std::size_t> m_files;
	ModuleDirectories               m_directories;
	const SourceFile               *sf_entry      = program.units.front()->getSourceUnit().getSource();
	const fs::path                 &p_libraryRoot = program.standardLibraryRoot;
	fs::path                        p_entryRoot;
	std::vector<fs::path>           v_sourceRoots;
	std::map<fs::path, std::string> m_moduleNames;
	for (const auto &[name, path] : modules.imports) {
		if (!m_moduleNames.emplace(fs::canonical(path), name).second)
			throw cde::ClothDiagnostic(Code::InvalidCompilationRequest, "A library source has multiple module names");
	}
	if (!modules.name.empty()) program.nativeModules.emplace(0, modules.name);
	if (sf_entry && !sf_entry->getPath().empty() && sf_entry->getPath() != "<input>") {
		const fs::path p_entry = fs::weakly_canonical(fs::absolute(sf_entry->getPath()));
		p_entryRoot            = p_entry.parent_path();
		m_files.emplace(p_entry, 0);
		if (!p_libraryRoot.empty() && p_entry.lexically_relative(fs::weakly_canonical(p_libraryRoot)) == CONSOLE_MODULE)
			m_libraryPaths[0].insert(std::string(CONSOLE_MODULE));

		if (m_moduleNames.contains(p_entry))
			throw cde::ClothDiagnostic(Code::InvalidCompilationRequest, "An external library refers to the entry source");
	}
	v_sourceRoots.push_back(p_entryRoot);
	for (const auto module : { NUMERIC_MODULE, PRIMITIVE_MODULE, PRELUDE_MODULE }) {
		if (p_libraryRoot.empty() || !fs::is_regular_file(p_libraryRoot / module)) continue;
		const fs::path path       = fs::canonical(p_libraryRoot / module);
		const auto [it, inserted] = m_files.emplace(path, program.units.size());
		m_libraryPaths[it->second].insert(std::string(module));
		if (inserted) {
			Lexer lexer{ SourceFile(path) };
			program.units.emplace_back(std::make_unique<SymbolTable>(Parser(lexer).parse()));
			v_sourceRoots.push_back(p_libraryRoot);
		}
		if (module == PRELUDE_MODULE) {
			preludeFile                 = it->second;
			v_sourceRoots[*preludeFile] = p_libraryRoot;
		}
	}
	const auto loadUnit = [&] (const fs::path &path, const fs::path &sourceRoot) {
		const auto [file, inserted] = m_files.emplace(path, program.units.size());
		if (inserted) {
			const auto module = m_moduleNames.find(path);
			if (module != m_moduleNames.end()) program.nativeModules.emplace(file->second, module->second);
			v_sourceRoots.push_back(module != m_moduleNames.end() ? path.parent_path() : sourceRoot);
			Lexer lexer{ SourceFile(path) };
			program.units.emplace_back(std::make_unique<SymbolTable>(Parser(lexer).parse()));
		}
		return file->second;
	};
	// A prebuilt may still call a manifest dependency that the current source no longer imports.
	if (modules.recordExternInterfaces) {
		for (const auto &[path, name] : m_moduleNames) loadUnit(path, path.parent_path());
	}
	for (std::size_t st_file = 0; st_file < program.units.size(); ++st_file) {
		v_names.emplace_back();
		v_edges.emplace_back();
		const SymbolTable &st_unit = *program.units[st_file];
		registerScope(st_unit.getGlobals(), st_file, nullptr);
		const fs::path          p_sourceRoot = v_sourceRoots[st_file];
		std::vector<ImportRoot> v_roots;
		if (std::error_code ec_root; p_libraryRoot.empty() || !fs::equivalent(p_sourceRoot, p_libraryRoot, ec_root))
			v_roots.push_back({ .path = p_sourceRoot, .standardLibrary = false });
		if (!p_libraryRoot.empty()) v_roots.push_back({ .path = p_libraryRoot, .standardLibrary = true });
		for (const ImportDeclaration &id_import : st_unit.getSourceUnit().imports) {
			if (p_sourceRoot.empty()) {
				fail(Code::MissingImportContext, id_import.path.getSpan(), "A source path is required to resolve imports");
			}
			fs::path    p_target;
			std::string s_libraryPath;
			const auto &v_parts = id_import.path.components;
			if (v_parts.size() == 1 && st_file != preludeFile) {
				const auto it_module = modules.imports.find(v_parts.front().getNameKey());
				if (it_module != modules.imports.end()) p_target = fs::canonical(it_module->second);
			}
			for (const ImportRoot &ir_root : v_roots) {
				if (!p_target.empty()) break;
				fs::path p_directory = ir_root.path;
				for (std::size_t st_index = 0; st_index + 1 < v_parts.size() && !p_directory.empty(); ++st_index) {
					const Token &t_part = v_parts[st_index];
					p_directory         = moduleEntry(m_directories, p_directory, t_part.getNameKey(), true, t_part.getSpan());
				}
				if (p_directory.empty()) continue;
				for (const char *extension : { ".co", ".clib" }) {
					const fs::path candidate = moduleEntry(m_directories, p_directory, v_parts.back().getNameKey() + extension, false,
					                                       id_import.path.getSpan());
					if (candidate.empty()) continue;
					if (!p_target.empty())
						fail(Code::AmbiguousImportSource, id_import.path.getSpan(), "Both .co and .clib match this import");
					p_target = fs::canonical(candidate);
					if (ir_root.standardLibrary) {
						for (const Token &t_part : v_parts) {
							if (!s_libraryPath.empty()) s_libraryPath += '/';
							s_libraryPath += t_part.getNameKey();
						}
						s_libraryPath += extension;
					}
				}
			}
			if (p_target.empty()) {
				fail(Code::ImportedSourceNotFound, id_import.path.getSpan(), "Cannot find source for '" + id_import.path.getSpelling() + "'");
			}
			const auto file = loadUnit(p_target, p_sourceRoot);
			if (!s_libraryPath.empty()) m_libraryPaths[file].insert(std::move(s_libraryPath));
			v_edges[st_file].emplace_back(&id_import, file);
		}
	}
	for (std::size_t st_file = 0; st_file < v_edges.size(); ++st_file) {
		for (const Edge &edge : v_edges[st_file]) {
			const SymbolScope &ss_exports = program.units[edge.second]->getGlobals();
			const auto         importName = [&] (const std::string &name, const Token &use, const bool explicitName) {
                                const auto it = ss_exports.find(name);
                                if (it == ss_exports.end()) {
                                        fail(Code::ImportNameNotFound, use.getSpan(), "Unknown imported name '" + use.getLexeme() + "'");
                                }
                                Candidates v_public;
                                for (const Symbol &sym : it->second) {
                                        if (sym.visibility == Visibility::Public) {
                                                v_public.push_back(&sym);
                                        }
                                }
                                if (v_public.empty()) {
                                        if (explicitName) {
                                                fail(Code::ImportPrivateName, use.getSpan(), "'" + use.getLexeme() + "' is private", &it->second.front().name);
                                        }
                                        return;
                                }
                                Candidates &v_slot = v_names[st_file][name].symbols;
                                if (!v_slot.empty() && v_slot != v_public) {
                                        const std::string &s_spelling = explicitName ? use.getLexeme() : v_public.front()->name.getLexeme();
                                        fail(Code::ImportNameConflict, use.getSpan(), "Conflicting import of '" + s_spelling + "'", &v_slot.front()->name);
                                }
                                v_slot = std::move(v_public);
			};
			if (const ImportSelection::Names *names = std::get_if<ImportSelection::Names>(&edge.first->selection.value)) {
				for (const Token &t_name : names->names) {
					importName(t_name.getNameKey(), t_name, true);
				}
			} else {
				for (const SymbolScope::value_type &entry : ss_exports) {
					importName(entry.first, edge.first->keyword, false);
				}
			}
		}
	}
	if (preludeFile) {
		// Install defaults after explicit imports have resolved, including their ordinary conflicts.
		for (const auto &[name, binding] : v_names[*preludeFile]) {
			FileNameBinding shared{ .symbols = {}, .fromPrelude = true };
			for (const Symbol *symbol : binding.symbols)
				if (symbol->visibility == Visibility::Public) shared.symbols.push_back(symbol);
			if (shared.symbols.empty()) continue;
			for (Names &scope : v_names) scope.try_emplace(name, shared);
		}
	}
}

Candidates Analyzer::lookup (const Symbol &context, const std::string &name, std::optional<SymbolKind> kind, bool includePrelude) const {
	for (const Symbol *sym = &context; sym; sym = declarations.at(sym).owner) {
		Candidates v_found;
		if (sym->kind == SymbolKind::Type && (!kind || *kind == SymbolKind::Type) && sym->name.getNameKey() == name) {
			v_found.push_back(sym);
		}
		if (const auto it_members = sym->members.find(name); it_members != sym->members.end()) {
			for (const Symbol &member : it_members->second) {
				if (!kind || member.kind == *kind) v_found.push_back(&member);
			}
		}
		if (!v_found.empty()) {
			return v_found;
		}
	}
	Candidates v_found = lookupFile(context, name, includePrelude);
	if (kind) std::erase_if(v_found, [&] (const Symbol *symbol) { return symbol->kind != *kind; });
	return v_found;
}

const Symbol *Analyzer::lookupTypeRoot (const Symbol &context, const std::string &name, bool qualified) const {
	if (const Candidates v_types = lookup(context, name, SymbolKind::Type); !v_types.empty()) return v_types.front();
	// A top-level state can own the first nested type in a dotted path.
	return qualified ? lookupFileState(context, name) : nullptr;
}

bool Analyzer::hasTypeName (const Symbol &context, const Token &name, bool qualified) const {
	return genericParameter(context, name.getNameKey()) || lookupTypeRoot(context, name.getNameKey(), qualified);
}

bool Analyzer::hasExplicitTypeName (const Symbol &context, const Token &name) const {
	return genericParameter(context, name.getNameKey()) || !lookup(context, name.getNameKey(), SymbolKind::Type, /*includePrelude=*/false).empty();
}

const Symbol *Analyzer::lookupFileState (const Symbol &context, const std::string &name) const {
	for (const Symbol *sym_candidate : lookupFile(context, name))
		if (sym_candidate->kind == SymbolKind::State) return sym_candidate;
	return nullptr;
}

Candidates Analyzer::lookupFile (const Symbol &context, const std::string &name, bool includePrelude) const {
	const Names &names = v_names[declarations.at(&context).file];
	const auto   it    = names.find(name);
	Candidates   v_found;
	if (it != names.end() && (includePrelude || !it->second.fromPrelude)) v_found = it->second.symbols;
	if (name == objectRoot().name.getNameKey()) v_found.push_back(&objectRoot());
	return v_found;
}

bool Analyzer::accessible (const Symbol &symbol, const Symbol &context) const {
	for (const Symbol *item = &symbol; item; item = declarations.at(item).owner) {
		if (item->visibility == Visibility::Public) {
			continue;
		}
		const DeclarationInfo &decl = declarations.at(item);
		if (!decl.owner) {
			return decl.file == declarations.at(&context).file;
		}
		const Symbol *scope = &context;
		while (scope && scope != decl.owner) {
			scope = declarations.at(scope).owner;
		}
		if (!scope) {
			return false;
		}
	}
	return true;
}

bool Analyzer::accessible (const Candidate &candidate, const Symbol &context) const {
	if (!candidate.dispatch || candidate.dispatch->kind != DispatchKind::State) return accessible(*candidate.symbol, context);
	const Symbol &sym_host = *candidate.dispatch->contract.owner.declaration;
	if (!accessible(sym_host, context)) return false;
	if (candidate.symbol->visibility == Visibility::Public || accessible(*candidate.symbol, context)) return true;
	for (const Symbol *sym_scope = &context; sym_scope; sym_scope = declarations.at(sym_scope).owner) {
		if (sym_scope == &sym_host) return true;
	}
	return false;
}

void Analyzer::requireAccess (const Symbol &symbol, const Symbol &context, const TokenSpan &use) const {
	if (!accessible(symbol, context)) {
		fail(symbol.kind == SymbolKind::Type ? Code::PrivateNestedType : Code::PrivateClassMember, use, "'" + symbol.name.getLexeme() + "' is private",
		     &symbol.name);
	}
}

void Analyzer::checkBodies () {
	collectFormatterClasses();
	for (const Symbol *sym_declaration : v_order) collectSignature(*sym_declaration);
	// Bind every body before typing any of them. Functions and methods each follow file and source order.
	Candidates v_bodies = v_order;
	std::ranges::stable_sort(v_bodies, {}, [&] (const Symbol *symbol) {
		const TokenSpan ts_name = symbol->name.getSpan();
		const unsigned  u_group = symbol->kind == SymbolKind::Function                                         ? 0u :
		                          (symbol->kind == SymbolKind::Method || symbol->kind == SymbolKind::Operator) ? 1u :
		                                                                                                         2u;
		return std::tuple{ u_group, declarations.at(symbol).file, ts_name.startLine, ts_name.startCol };
	});
	for (const Symbol *sym : v_bodies) {
		if (const auto it_hosts = m_stateHosts.find(standaloneState(*sym)); it_hosts == m_stateHosts.end()) {
			BodyChecker(*this, *sym, program, {}, declarations.at(sym)).resolveDeclarationNames();
		} else {
			for (const ResolvedType &rt_host : it_hosts->second)
				BodyChecker(*this, *sym, program, {}, declarations.at(sym), &rt_host).resolveDeclarationNames();
		}
	}
	for (const Symbol *sym : v_bodies) {
		if (sym->classFieldIndex) {
			continue;
		}
		if (const TypeDeclaration *const *type = std::get_if<const TypeDeclaration *>(&sym->declaration)) {
			if (const auto *enumeration = dynamic_cast<const EnumDeclaration *>(*type)) {
				for (const EnumFieldDeclaration &field : enumeration->fields)
					checkBodyOf(*std::ranges::find(sym->members.at(field.name.getNameKey()), SymbolKind::Field, &Symbol::kind));
				for (const EnumCaseDeclaration &entry : enumeration->cases)
					checkBodyOf(*std::ranges::find(sym->members.at(entry.name.getNameKey()), SymbolKind::EnumCase, &Symbol::kind));
			}
		}
		const Symbol *sym_primary = declarations.at(sym).primaryConstructor;
		if (sym_primary) {
			checkBodyOf(*sym_primary);
		}
		checkBodyOf(*sym);
		if (!sym_primary) {
			continue;
		}
		const auto &cd_class = static_cast<const ClassDeclaration &>(*std::get<const TypeDeclaration *>(sym->declaration));
		// The primary checked its defaults and the class checked its base call. Field initializers now follow source order.
		for (const ClassMember &cm_member : cd_class.members) {
			if (const ClassFieldDeclaration *cfd_field = std::get_if<ClassFieldDeclaration>(&cm_member.value)) {
				const std::vector<Symbol> &v_group = sym->members.at(cfd_field->name.getNameKey());
				checkBodyOf(*std::ranges::find(v_group, SymbolKind::Field, &Symbol::kind));
			}
		}
	}
	checkWidthAnnotations();
	checkFormatterDefaults();
	checkInstances();
	checkFormatterDefaults();
	// Expand dependencies first so recursively growing bounds retain their template-limit diagnostic.
	for (const Symbol *sym_declaration : v_order) {
		if (const TypeParameterList *tpl_parameters = typeParameters(*sym_declaration))
			for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters)
				for (const TypeParameterBound &tp_bound : tp_parameter.bounds)
					if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value))
						checkBounds(type(*tr_bound, *sym_declaration), tr_bound->getSpan(), *sym_declaration);
	}
	publishDispatch();
	checkLifetimes();
}

const Symbol *Analyzer::standaloneState (const Symbol &member) const {
	const Symbol *sym_state = declarations.at(&member).owner;
	return sym_state && sym_state->kind == SymbolKind::State && !declarations.at(sym_state).owner ? sym_state : nullptr;
}

const BodyFacts &Analyzer::openBody (const ResolvedType *attachment) const {
	return attachment ? program.attachmentBodies.at(ownerType(*attachment->declaration)) : static_cast<const BodyFacts &>(program);
}

void Analyzer::checkAttachedBody (const Symbol &member, const ResolvedType &host, BodyFacts &facts, const Substitutions &arguments,
                                  DeclarationInfo &signature) {
	const ResolvedType *rt_previous = std::exchange(rt_activeAttachment, &host);
	try {
		BodyChecker(*this, member, facts, arguments, signature, &host).check();
	} catch (...) {
		rt_activeAttachment = rt_previous;
		throw;
	}
	rt_activeAttachment = rt_previous;
}

void Analyzer::checkBodyOf (const Symbol &symbol) {
	if (s_checked.contains(&symbol)) {
		return;
	}
	if (!s_checking.insert(&symbol).second) {
		fail(Code::InvalidOperatorDeclaration, symbol.name.getSpan(), "Recursive operator needs an explicit result type");
	}
	try {
		const auto it_hosts = m_stateHosts.find(standaloneState(symbol));
		if (const TypeReference *tr_width = widthSelector(symbol)) {
			if (isTemplate(symbol)) widthTemplate(symbol, tr_width->getName().getLexeme() == toString(KInteger) ? MAX_INTEGER_BITS : 64u);
		} else if (it_hosts != m_stateHosts.end()) {
			for (const ResolvedType &rt_host : it_hosts->second)
				checkAttachedBody(symbol, rt_host, program.attachmentBodies[rt_host], {}, declarations.at(&symbol));
		} else {
			BodyChecker(*this, symbol, program, {}, declarations.at(&symbol)).check();
		}
		if (symbol.kind == SymbolKind::Operator) checkPublicSurface(symbol);
	} catch (const cde::ClothDiagnostic &) {
		s_checking.erase(&symbol);
		checkFormatterDefaults();
		throw;
	} catch (...) {
		s_checking.erase(&symbol);
		throw;
	}
	s_checking.erase(&symbol);
	s_checked.insert(&symbol);
}

const ResultTypes &Analyzer::resultsOf (const Symbol &symbol) {
	if (declarations.at(&symbol).inferResult) {
		checkBodyOf(symbol);
	}
	return declarations.at(&symbol).returns;
}

ResultTypes Analyzer::callResults (const Symbol &symbol, const Substitutions &arguments, unsigned operatorWidth) {
	if (operatorWidth) return widthSignature(symbol, arguments, operatorWidth).returns;
	ResultTypes results;
	for (const ResolvedType &type : resultsOf(symbol)) results.push_back(substitute(type, arguments));
	return results;
}
}
