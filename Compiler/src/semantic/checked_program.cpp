// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <filesystem>
#include <format>

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
	std::string s_result = name;
	if (width && width != 32) {
		s_result += std::format("{}{}{}", to_string(oLess), width, to_string(oGreater));
	}
	if (!arguments.empty()) {
		s_result += to_string(oLess);
		for (const ResolvedType &rt_arg : arguments) {
			if (&rt_arg != &arguments.front()) {
				s_result += to_string(oComma);
				s_result += ' ';
			}
			s_result += rt_arg.spelling();
		}
		s_result += to_string(oGreater);
	}
	for (const std::string &s_suffix : suffixes) {
		s_result += s_suffix;
	}
	return s_result;
}

CheckedProgram::CheckedProgram (SymbolTable entry) {
	units.emplace_back(std::make_unique<SymbolTable>(std::move(entry)));
	semantic::Analyzer analyzer(*this);
	analyzer.checkBodies();
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

ResolvedType substitute (ResolvedType type, const Substitutions &arguments) {
	if (const auto it = arguments.find(type.parameter); it != arguments.end()) {
		ResolvedType rt_result = it->second;
		rt_result.suffixes.insert(rt_result.suffixes.end(), type.suffixes.begin(), type.suffixes.end());
		return rt_result;
	}
	for (ResolvedType &rt_arg : type.arguments) {
		rt_arg = substitute(std::move(rt_arg), arguments);
	}
	return type;
}

Analyzer::Analyzer (CheckedProgram &program) : program(program), declarations(program.declarations) {
	loadImports();
	for (const Symbol *sym : v_order) {
		resolveDeclaration(*sym);
	}
	checkDeclarations();
}

void Analyzer::registerScope (const SymbolScope &scope, std::size_t file, const Symbol *owner) {
	for (const SymbolScope::value_type &entry : scope) {
		for (const Symbol &sym : entry.second) {
			declarations.emplace(&sym, DeclarationInfo{ file, owner });
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
	std::vector<std::vector<Edge>>  v_edges;
	std::map<fs::path, std::size_t> m_files;
	const SourceFile               *sf_entry = program.units.front()->get_source_unit().get_source();
	fs::path                        p_root;
	if (sf_entry && !sf_entry->get_path().empty() && sf_entry->get_path() != "<input>") {
		const fs::path p_entry = fs::weakly_canonical(fs::absolute(sf_entry->get_path()));
		p_root                 = p_entry.parent_path();
		m_files.emplace(p_entry, 0);
	}
	for (std::size_t st_file = 0; st_file < program.units.size(); ++st_file) {
		v_names.emplace_back();
		v_edges.emplace_back();
		const SymbolTable &st_unit = *program.units[st_file];
		registerScope(st_unit.get_globals(), st_file, nullptr);
		for (const ImportDeclaration &id_import : st_unit.get_source_unit().imports) {
			if (p_root.empty()) {
				fail(Code::MISSING_IMPORT_CONTEXT, id_import.path.get_span(), "A source path is required to resolve imports");
			}
			fs::path p_base = p_root;
			for (const Token &t_part : id_import.path.components) {
				p_base /= t_part.get_lexeme();
			}
			fs::path p_target;
			for (const char *s_extension : { ".co", ".clib" }) {
				fs::path p_candidate = p_base;
				p_candidate += s_extension;
				if (!fs::is_regular_file(p_candidate)) {
					continue;
				}
				if (!p_target.empty()) {
					fail(Code::AMBIGUOUS_IMPORT_SOURCE, id_import.path.get_span(), "Both .co and .clib match this import");
				}
				p_target = fs::canonical(p_candidate);
			}
			if (p_target.empty()) {
				fail(Code::IMPORTED_SOURCE_NOT_FOUND, id_import.path.get_span(),
				     "Cannot find source for '" + id_import.path.get_spelling() + "'");
			}
			const std::pair<decltype(m_files)::iterator, bool> inserted = m_files.emplace(p_target, program.units.size());
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

Candidates Analyzer::lookup (const Symbol &context, const std::string &name) const {
	for (const Symbol *sym = &context; sym; sym = declarations.at(sym).owner) {
		Candidates v_found;
		if (sym->kind == SymbolKind::Type && sym->name.get_lexeme() == name) {
			v_found.push_back(sym);
		}
		if (const auto it = sym->members.find(name); it != sym->members.end()) {
			for (const Symbol &member : it->second) {
				v_found.push_back(&member);
			}
		}
		if (!v_found.empty()) {
			return v_found;
		}
	}
	const Names &names = v_names[declarations.at(&context).file];
	const auto   it    = names.find(name);
	return it == names.end() ? Candidates{} : it->second;
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
			if (const DeclarationInfo &enclosing = declarations.at(scope);
			    scope->kind == SymbolKind::State && !enclosing.owner && !enclosing.parents.empty()) {
				scope = enclosing.parents.front().declaration;
			} else {
				scope = enclosing.owner;
			}
		}
		if (!scope) {
			return false;
		}
	}
	return true;
}

void Analyzer::requireAccess (const Symbol &symbol, const Symbol &context, const TokenSpan &use) const {
	if (!accessible(symbol, context)) {
		fail(symbol.kind == SymbolKind::Type ? Code::PRIVATE_NESTED_TYPE : Code::PRIVATE_CLASS_MEMBER, use,
		     "'" + symbol.name.get_lexeme() + "' is private", &symbol.name);
	}
}

void Analyzer::checkBodies () {
	for (const Symbol *sym : v_order) {
		checkBodyOf(*sym);
	}
}

void Analyzer::checkBodyOf (const Symbol &symbol) {
	if (s_checked.contains(&symbol)) {
		return;
	}
	if (!s_checking.insert(&symbol).second) {
		fail(Code::INVALID_OPERATOR_DECLARATION, symbol.name.get_span(), "Recursive operator needs an explicit result type");
	}
	try {
		BodyChecker(*this, symbol).check();
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
