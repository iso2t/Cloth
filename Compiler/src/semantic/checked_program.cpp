// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <ast/decl/class_declaration.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <semantic/checker.hpp>
#include <token/token.hpp>

namespace cloth {

ResolvedType::ResolvedType (Keyword keyword) : ResolvedType(std::string(to_string(keyword))) {
}

ResolvedType::ResolvedType (std::string name) : name(std::move(name)) {
	if (this->name == to_string(kInteger) || this->name == semantic::unsignedIntegerSpelling() || this->name == to_string(kFloat)) {
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
	return (*this <=> other) == 0;
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
	return !suffixes.empty() && suffixes.back() == to_string(oQuestion);
}

bool ResolvedType::array () const {
	return !suffixes.empty() && suffixes.back().starts_with(to_string(oBracketOpen));
}

bool ResolvedType::integer () const {
	return suffixes.empty() &&
	       (name == to_string(kInteger) || name == semantic::unsignedIntegerSpelling() || name == to_string(kUsize) || name == to_string(kIsize));
}

bool ResolvedType::floating () const {
	return suffixes.empty() && name == to_string(kFloat);
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
		append(type.name);
		if (type.width && type.width != 32) append(std::format("{}{}{}", to_string(oLess), type.width, to_string(oGreater)));
		if (!type.arguments().empty()) {
			append(to_string(oLess));
			for (const ResolvedType &rt_argument : type.arguments()) {
				if (b_truncated) return;
				if (&rt_argument != &type.arguments().front()) {
					append(to_string(oComma));
					append(" ");
				}
				write(rt_argument);
			}
			append(to_string(oGreater));
		}
		auto extent = type.suffixes.rbegin();
		for (const std::string &suffix : type.suffixes) {
			if (suffix == to_string(oQuestion)) {
				append(suffix);
			} else {
				while (*extent == to_string(oQuestion)) ++extent;
				append(*extent++);
			}
		}
	};
	write(*this);
	return s_result;
}

std::optional<ResolvedType> ExpressionInfo::representedAt (std::size_t slot) const {
	if (slot < representedResults.size()) return representedResults[slot];
	return slot == 0 ? representedType : std::nullopt;
}

ResultKind ExpressionInfo::resultKind () const {
	if (!fallsThrough) return ResultKind::Never;
	if (types.empty()) return ResultKind::Empty;
	return types.size() == 1 ? ResultKind::Single : ResultKind::Multiple;
}

ExpressionInfo ExpressionInfo::never () {
	return { .types = {}, .fallsThrough = false };
}

CheckedProgram::CheckedProgram (SymbolTable entry, TargetLayout layout, std::filesystem::path standardLibraryRoot, ProgramKind kind) :
        layout(layout),
        standardLibraryRoot(std::move(standardLibraryRoot)),
        kind(kind) {
	if (layout.sizeBits != 32 && layout.sizeBits != 64) throw std::invalid_argument("A Cloth target requires a 32- or 64-bit size type");
	units.emplace_back(std::make_unique<SymbolTable>(std::move(entry)));
	semantic::Analyzer analyzer(*this);
	analyzer.checkBodies();
	entryPoint = analyzer.entryPoint();
}
}

namespace cloth::semantic {

std::string_view unsignedIntegerSpelling () {
	static const std::string S_UNSIGNED_INTEGER = to_stringmulti({ kUnsigned, kInteger });
	return S_UNSIGNED_INTEGER;
}

std::string arraySuffix (std::string_view extent) {
	std::string s_suffix(to_string(oBracketOpen));
	s_suffix += extent;
	s_suffix += to_string(oBracketClose);
	return s_suffix;
}

void fail (const Code code, const TokenSpan &span, std::string message, const Token *related) {
	std::vector<cde::SourceLabel> v_labels{ { span, message } };
	if (related) {
		v_labels.push_back({ related->get_span(), "declared here" });
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
		const TypeDeclaration *td_type = std::get<const TypeDeclaration *>(symbol.declaration);
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

Analyzer::Analyzer (CheckedProgram &program) : program(program), declarations(program.declarations) {
	declarations.emplace(&objectRoot(), DeclarationInfo{ 0, nullptr });
	loadImports();
	registerIntrinsics();
	for (const Symbol *sym : v_order) checkParameterNames(*sym);
	for (const Symbol *sym : v_order) {
		resolveDeclaration(*sym);
		if (const std::optional<LibraryIntrinsic> op = declarations.at(sym).intrinsic) checkIntrinsicSignature(*sym, *op);
	}
	checkDeclarations();
}

void Analyzer::registerScope (const SymbolScope &scope, std::size_t file, const Symbol *owner) {
	for (const SymbolScope::value_type &entry : scope) {
		for (const Symbol &sym : entry.second) {
			if (sym.kind == SymbolKind::Type && sym.name.get_lexeme() == objectRoot().name.get_lexeme())
				fail(Code::INVALID_CLASS_MEMBER, sym.name.get_span(), std::format("{} is the built-in class root", sym.name.get_lexeme()));
			declarations.emplace(&sym, DeclarationInfo{ file, owner });
			if (!owner && entry.first == "Number" && sym.visibility == Visibility::Public && asInterface(&sym) && !typeParameters(sym) &&
			    !program.standardLibraryRoot.empty()) {
				const SourceFile *sf_source = program.units[file]->get_source_unit().get_source();
				std::error_code   ec_file;
				if (sf_source &&
				    std::filesystem::equivalent(sf_source->get_path(), program.standardLibraryRoot / "cloth/lang/num/Number.co", ec_file))
					sym_number = &sym;
			}
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
							declarations.emplace(&requirement, DeclarationInfo{ file, &sym });
							m_guarantees[&parameter].push_back(&requirement);
							v_order.push_back(&requirement);
						}
					}
				}
			}
			if (!owner) {
				v_names[file][entry.first].push_back(&sym);
			}
			registerScope(sym.members, file, &sym);
		}
	}
}

void Analyzer::loadImports () {
	namespace fs = std::filesystem;
	using Edge   = std::pair<const ImportDeclaration *, std::size_t>;

	struct ImportRoot {
		const fs::path &path;
		bool            standardLibrary;
	};

	std::vector<std::vector<Edge>>  v_edges;
	std::map<fs::path, std::size_t> m_files;
	const SourceFile               *sf_entry = program.units.front()->get_source_unit().get_source();
	fs::path                        p_root;
	if (sf_entry && !sf_entry->get_path().empty() && sf_entry->get_path() != "<input>") {
		const fs::path p_entry = fs::weakly_canonical(fs::absolute(sf_entry->get_path()));
		p_root                 = p_entry.parent_path();
		m_files.emplace(p_entry, 0);
	}
	const ImportRoot ir_roots[] = { { p_root, false }, { program.standardLibraryRoot, true } };
	for (std::size_t st_file = 0; st_file < program.units.size(); ++st_file) {
		v_names.emplace_back();
		v_edges.emplace_back();
		const SymbolTable &st_unit = *program.units[st_file];
		registerScope(st_unit.get_globals(), st_file, nullptr);
		for (const ImportDeclaration &id_import : st_unit.get_source_unit().imports) {
			if (p_root.empty()) {
				fail(Code::MISSING_IMPORT_CONTEXT, id_import.path.get_span(), "A source path is required to resolve imports");
			}
			fs::path p_relative;
			for (const Token &part : id_import.path.components) p_relative /= part.get_lexeme();
			fs::path    p_target;
			std::string s_libraryPath;
			for (const ImportRoot &ir_root : ir_roots) {
				if (ir_root.path.empty()) continue;
				for (const char *extension : { ".co", ".clib" }) {
					fs::path candidate = ir_root.path / p_relative;
					candidate += extension;
					if (!fs::is_regular_file(candidate)) continue;
					if (!p_target.empty())
						fail(Code::AMBIGUOUS_IMPORT_SOURCE, id_import.path.get_span(), "Both .co and .clib match this import");
					p_target = fs::canonical(candidate);
					if (ir_root.standardLibrary) s_libraryPath = p_relative.generic_string() + extension;
				}
				if (!p_target.empty()) break;
			}
			if (p_target.empty()) {
				fail(Code::IMPORTED_SOURCE_NOT_FOUND, id_import.path.get_span(),
				     "Cannot find source for '" + id_import.path.get_spelling() + "'");
			}
			const std::pair<decltype(m_files)::iterator, bool> inserted = m_files.emplace(p_target, program.units.size());
			if (!s_libraryPath.empty()) m_libraryPaths[inserted.first->second].insert(std::move(s_libraryPath));
			if (inserted.second) {
				Lexer l_lexer{ SourceFile(p_target) };
				program.units.emplace_back(std::make_unique<SymbolTable>(Parser(l_lexer).parse()));
			}
			v_edges[st_file].emplace_back(&id_import, inserted.first->second);
		}
	}
	for (std::size_t st_file = 0; st_file < v_edges.size(); ++st_file) {
		for (const Edge &edge : v_edges[st_file]) {
			const SymbolScope &ss_exports = program.units[edge.second]->get_globals();
			const auto         importName = [&] (const std::string &name, const Token &use, const bool explicitName) {
                                const auto it = ss_exports.find(name);
                                if (it == ss_exports.end()) {
                                        fail(Code::IMPORT_NAME_NOT_FOUND, use.get_span(), "Unknown imported name '" + name + "'");
                                }
                                Candidates v_public;
                                for (const Symbol &sym : it->second) {
                                        if (sym.visibility == Visibility::Public) {
                                                v_public.push_back(&sym);
                                        }
                                }
                                if (v_public.empty()) {
                                        if (explicitName) {
                                                fail(Code::IMPORT_PRIVATE_NAME, use.get_span(), "'" + name + "' is private", &it->second.front().name);
                                        }
                                        return;
                                }
                                Candidates &v_slot = v_names[st_file][name];
                                if (!v_slot.empty() && v_slot != v_public) {
                                        fail(Code::IMPORT_NAME_CONFLICT, use.get_span(), "Conflicting import of '" + name + "'", &v_slot.front()->name);
                                }
                                v_slot = std::move(v_public);
			};
			if (const ImportSelection::Names *names = std::get_if<ImportSelection::Names>(&edge.first->selection.value)) {
				for (const Token &t_name : names->names) {
					importName(t_name.get_lexeme(), t_name, true);
				}
			} else {
				for (const SymbolScope::value_type &entry : ss_exports) {
					importName(entry.first, edge.first->keyword, false);
				}
			}
		}
	}
}

Candidates Analyzer::lookup (const Symbol &context, const std::string &name, std::optional<SymbolKind> kind) const {
	for (const Symbol *sym = &context; sym; sym = declarations.at(sym).owner) {
		Candidates v_found;
		if (sym->kind == SymbolKind::Type && (!kind || *kind == SymbolKind::Type) && sym->name.get_lexeme() == name) {
			v_found.push_back(sym);
		}
		if (const SymbolScope::const_iterator it_members = sym->members.find(name); it_members != sym->members.end()) {
			for (const Symbol &member : it_members->second) {
				if (!kind || member.kind == *kind) v_found.push_back(&member);
			}
		}
		if (!v_found.empty()) {
			return v_found;
		}
	}
	Candidates v_found = lookupFile(context, name);
	if (kind) std::erase_if(v_found, [&] (const Symbol *symbol) { return symbol->kind != *kind; });
	return v_found;
}

const Symbol *Analyzer::lookupType (const Symbol &context, const std::string &name) const {
	const Candidates v_types = lookup(context, name, SymbolKind::Type);
	return v_types.empty() ? nullptr : v_types.front();
}

bool Analyzer::hasTypeName (const Symbol &context, const Token &name) const {
	return genericParameter(context, name.get_lexeme()) || lookupType(context, name.get_lexeme());
}

Candidates Analyzer::lookupFile (const Symbol &context, const std::string &name) const {
	const Names &names   = v_names[declarations.at(&context).file];
	const auto   it      = names.find(name);
	Candidates   v_found = it == names.end() ? Candidates{} : it->second;
	if (name == objectRoot().name.get_lexeme()) v_found.push_back(&objectRoot());
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
		fail(symbol.kind == SymbolKind::Type ? Code::PRIVATE_NESTED_TYPE : Code::PRIVATE_CLASS_MEMBER, use,
		     "'" + symbol.name.get_lexeme() + "' is private", &symbol.name);
	}
}

void Analyzer::checkBodies () {
	for (const Symbol *sym_declaration : v_order) collectSignature(*sym_declaration);
	for (const Symbol *sym : v_order) {
		if (sym->classFieldIndex) {
			continue;
		}
		if (const TypeDeclaration *const *type = std::get_if<const TypeDeclaration *>(&sym->declaration)) {
			if (const EnumDeclaration *enumeration = dynamic_cast<const EnumDeclaration *>(*type)) {
				for (const EnumFieldDeclaration &field : enumeration->fields)
					checkBodyOf(*std::ranges::find(sym->members.at(field.name.get_lexeme()), SymbolKind::Field, &Symbol::kind));
				for (const EnumCaseDeclaration &entry : enumeration->cases)
					checkBodyOf(*std::ranges::find(sym->members.at(entry.name.get_lexeme()), SymbolKind::EnumCase, &Symbol::kind));
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
		const ClassDeclaration &cd_class = static_cast<const ClassDeclaration &>(*std::get<const TypeDeclaration *>(sym->declaration));
		// The primary checked its defaults and the class checked its base call. Field initializers now follow source order.
		for (const ClassMember &cm_member : cd_class.members) {
			if (const ClassFieldDeclaration *cfd_field = std::get_if<ClassFieldDeclaration>(&cm_member.value)) {
				const std::vector<Symbol> &v_group = sym->members.at(cfd_field->name.get_lexeme());
				checkBodyOf(*std::ranges::find(v_group, SymbolKind::Field, &Symbol::kind));
			}
		}
	}
	checkInstances();
	// Expand dependencies first so recursively growing bounds retain their template-limit diagnostic.
	for (const Symbol *sym_declaration : v_order) {
		if (const TypeParameterList *tpl_parameters = typeParameters(*sym_declaration))
			for (const TypeParameterDeclaration &tp_parameter : tpl_parameters->parameters)
				for (const TypeParameterBound &tp_bound : tp_parameter.bounds)
					if (const TypeReference *tr_bound = std::get_if<TypeReference>(&tp_bound.value))
						checkBounds(type(*tr_bound, *sym_declaration), tr_bound->get_span(), *sym_declaration);
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
		fail(Code::INVALID_OPERATOR_DECLARATION, symbol.name.get_span(), "Recursive operator needs an explicit result type");
	}
	try {
		const std::unordered_map<const Symbol *, ResultTypes>::const_iterator it_hosts = m_stateHosts.find(standaloneState(symbol));
		if (it_hosts != m_stateHosts.end()) {
			for (const ResolvedType &rt_host : it_hosts->second)
				checkAttachedBody(symbol, rt_host, program.attachmentBodies[rt_host], {}, declarations.at(&symbol));
		} else {
			BodyChecker(*this, symbol, program, {}, declarations.at(&symbol)).check();
		}
		if (symbol.kind == SymbolKind::Operator) checkPublicSurface(symbol);
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

}
